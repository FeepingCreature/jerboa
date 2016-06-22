#include "vm/runtime.h"

#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>

#include "vm/call.h"
#include "vm/ffi.h"
#include "gc.h"

#include <libxml/parser.h>
#include <libxml/tree.h>

static void my_asprintf(char **outp, char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int len = vsnprintf(NULL, 0, fmt, ap);
  *outp = malloc(len + 1);
  va_end(ap);
  va_start(ap, fmt);
  vsnprintf(*outp, len + 1, fmt, ap);
  va_end(ap);
}

static void bool_not_fn(VMState *state, Object *thisptr, Object *fn, Object **args_ptr, int args_len) {
  VM_ASSERT(args_len == 0, "wrong arity: expected 0, got %i", args_len);
  Object *root = state->root;
  Object *bool_base = object_lookup(root, "bool", NULL);
  BoolObject *boolobj = (BoolObject*) obj_instance_of(thisptr, bool_base);
  VM_ASSERT(boolobj, "internal error: bool negation called on wrong type of object");
  
  state->result_value = alloc_bool(state, !boolobj->value);
}

typedef enum {
  MATH_ADD,
  MATH_SUB,
  MATH_MUL,
  MATH_DIV
} MathOp;

static void int_math_fn(VMState *state, Object *thisptr, Object *fn, Object **args_ptr, int args_len, MathOp mop) {
  VM_ASSERT(args_len == 1, "wrong arity: expected 1, got %i", args_len);
  Object *root = state->root;
  
  Object *int_base = object_lookup(root, "int", NULL);
  Object
    *iobj1 = obj_instance_of(thisptr, int_base),
    *iobj2 = obj_instance_of(args_ptr[0], int_base);
  VM_ASSERT(iobj1, "internal error: int math function called on wrong type of object"); // otherwise how are we called on it??
  if (iobj2) {
    int i1 = ((IntObject*) iobj1)->value, i2 = ((IntObject*) iobj2)->value;
    int res;
    switch (mop) {
      case MATH_ADD: res = i1 + i2; break;
      case MATH_SUB: res = i1 - i2; break;
      case MATH_MUL: res = i1 * i2; break;
      case MATH_DIV:
        VM_ASSERT(i2 != 0, "division by zero");
        res = i1 / i2;
        break;
      default: assert(false);
    }
    state->result_value = alloc_int(state, res);
    return;
  }
  
  Object *float_base = object_lookup(root, "float", NULL);
  Object *fobj2 = obj_instance_of(args_ptr[0], float_base);
  if (fobj2) {
    float v1 = ((IntObject*) iobj1)->value, v2 = ((IntObject*) fobj2)->value;
    float res;
    switch (mop) {
      case MATH_ADD: res = v1 + v2; break;
      case MATH_SUB: res = v1 - v2; break;
      case MATH_MUL: res = v1 * v2; break;
      case MATH_DIV:
        VM_ASSERT(v2 != 0.0f, "float division by zero");
        res = v1 / v2;
        break;
      default: assert(false);
    }
    state->result_value = alloc_float(state, res);
    return;
  }
  vm_error(state, "don't know how to perform int math with %p", args_ptr[0]);
}

static void int_add_fn(VMState *state, Object *thisptr, Object *fn, Object **args_ptr, int args_len) {
  int_math_fn(state, thisptr, fn, args_ptr, args_len, MATH_ADD);
}

static void int_sub_fn(VMState *state, Object *thisptr, Object *fn, Object **args_ptr, int args_len) {
  int_math_fn(state, thisptr, fn, args_ptr, args_len, MATH_SUB);
}

static void int_mul_fn(VMState *state, Object *thisptr, Object *fn, Object **args_ptr, int args_len) {
  int_math_fn(state, thisptr, fn, args_ptr, args_len, MATH_MUL);
}

static void int_div_fn(VMState *state, Object *thisptr, Object *fn, Object **args_ptr, int args_len) {
  int_math_fn(state, thisptr, fn, args_ptr, args_len, MATH_DIV);
}

static void int_parse_fn(VMState *state, Object *thisptr, Object *fn, Object **args_ptr, int args_len) {
  VM_ASSERT(args_len == 1, "wrong arity: expected 1, got %i", args_len);
  Object *root = state->root;
  
  Object *string_base = object_lookup(root, "string", NULL);
  
  StringObject *sobj = (StringObject*) obj_instance_of(args_ptr[0], string_base);
  VM_ASSERT(sobj, "parameter to int.parse() must be string!");
  char *text = sobj->value;
  int base = 10;
  if (text[0] && text[0] == '-') text++;
  if (text[0] == '0' && text[1] == 'x') {
    base = 16;
    text += 2;
  }
  text = sobj->value;
  int res = (int) strtol(text, NULL, base);
  state->result_value = alloc_int(state, res);
}

static void float_math_fn(VMState *state, Object *thisptr, Object *fn, Object **args_ptr, int args_len, MathOp mop) {
  VM_ASSERT(args_len == 1, "wrong arity: expected 1, got %i", args_len);
  Object *root = state->root;
  Object *int_base = object_lookup(root, "int", NULL);
  Object *float_base = object_lookup(root, "float", NULL);
  
  Object
    *fobj1 = obj_instance_of(thisptr, float_base),
    *iobj2 = obj_instance_of(args_ptr[0], int_base),
    *fobj2 = obj_instance_of(args_ptr[0], float_base);
  VM_ASSERT(fobj1, "internal error: float math function called on wrong type of object");
  if (fobj2 || iobj2) {
    float v1 = ((FloatObject*) fobj1)->value, v2;
    if (fobj2) v2 = ((FloatObject*) fobj2)->value;
    else v2 = ((IntObject*) iobj2)->value;
    float res;
    switch (mop) {
      case MATH_ADD: res = v1 + v2; break;
      case MATH_SUB: res = v1 - v2; break;
      case MATH_MUL: res = v1 * v2; break;
      case MATH_DIV: res = v1 / v2; break;
      default: assert(false);
    }
    state->result_value = alloc_float(state, res);
    return;
  }
  vm_error(state, "don't know how to perform float math with %p", args_ptr[0]);
}

static void float_add_fn(VMState *state, Object *thisptr, Object *fn, Object **args_ptr, int args_len) {
  float_math_fn(state, thisptr, fn, args_ptr, args_len, MATH_ADD);
}

static void float_sub_fn(VMState *state, Object *thisptr, Object *fn, Object **args_ptr, int args_len) {
  float_math_fn(state, thisptr, fn, args_ptr, args_len, MATH_SUB);
}

static void float_mul_fn(VMState *state, Object *thisptr, Object *fn, Object **args_ptr, int args_len) {
  float_math_fn(state, thisptr, fn, args_ptr, args_len, MATH_MUL);
}

static void float_div_fn(VMState *state, Object *thisptr, Object *fn, Object **args_ptr, int args_len) {
  float_math_fn(state, thisptr, fn, args_ptr, args_len, MATH_DIV);
}

static void string_add_fn(VMState *state, Object *thisptr, Object *fn, Object **args_ptr, int args_len) {
  VM_ASSERT(args_len == 1, "wrong arity: expected 1, got %i", args_len);
  Object *root = state->root;
  Object *int_base = object_lookup(root, "int", NULL);
  Object *bool_base = object_lookup(root, "bool", NULL);
  Object *float_base = object_lookup(root, "float", NULL);
  Object *string_base = object_lookup(root, "string", NULL);
  
  Object
    *sobj1 = obj_instance_of(thisptr, string_base),
    *iobj2 = obj_instance_of(args_ptr[0], int_base),
    *bobj2 = obj_instance_of(args_ptr[0], bool_base),
    *fobj2 = obj_instance_of(args_ptr[0], float_base),
    *sobj2 = obj_instance_of(args_ptr[0], string_base);
  VM_ASSERT(sobj1, "internal error: string concat function called on wrong type of object");
  
  char *str1 = ((StringObject*) sobj1)->value, *str2;
  if (sobj2) my_asprintf(&str2, "%s", ((StringObject*) sobj2)->value);
  else if (fobj2) my_asprintf(&str2, "%f", ((FloatObject*) fobj2)->value);
  else if (iobj2) my_asprintf(&str2, "%i", ((IntObject*) iobj2)->value);
  else if (bobj2) if (((BoolObject*)bobj2)->value) my_asprintf(&str2, "%s", "true"); else my_asprintf(&str2, "%s", "false");
  else VM_ASSERT(false, "don't know how to format object: %p", args_ptr[0]);
  char *str3;
  my_asprintf(&str3, "%s%s", str1, str2);
  free(str2);
  state->result_value = alloc_string(state, str3);
  free(str3);
}

static void string_eq_fn(VMState *state, Object *thisptr, Object *fn, Object **args_ptr, int args_len) {
  VM_ASSERT(args_len == 1, "wrong arity: expected 1, got %i", args_len);
  Object *root = state->root;
  Object *string_base = object_lookup(root, "string", NULL);
  
  Object
    *sobj1 = obj_instance_of(thisptr, string_base),
    *sobj2 = obj_instance_of(args_ptr[0], string_base);
  VM_ASSERT(sobj1, "internal error: string compare function called on wrong type of object");
  VM_ASSERT(sobj2, "can only compare strings with strings!");
  
  char
    *str1 = ((StringObject*) sobj1)->value,
    *str2 = ((StringObject*) sobj2)->value;
  int res = strcmp(str1, str2);
  state->result_value = alloc_bool(state, res == 0);
}

typedef enum {
  CMP_EQ,
  CMP_LT,
  CMP_GT,
  CMP_LE,
  CMP_GE
} CompareOp;

static void int_cmp_fn(VMState *state, Object *thisptr, Object *fn, Object **args_ptr, int args_len, CompareOp cmp) {
  VM_ASSERT(args_len == 1, "wrong arity: expected 1, got %i", args_len);
  Object *root = state->root;
  
  Object *int_base = object_lookup(root, "int", NULL);
  Object
    *iobj1 = obj_instance_of(thisptr, int_base),
    *iobj2 = obj_instance_of(args_ptr[0], int_base);
  VM_ASSERT(iobj1, "internal error: int compare function called on wrong type of object");
  if (iobj2) {
    int i1 = ((IntObject*) iobj1)->value, i2 = ((IntObject*) iobj2)->value;
    bool res;
    switch (cmp) {
      case CMP_EQ: res = i1 == i2; break;
      case CMP_LT: res = i1 <  i2; break;
      case CMP_GT: res = i1 >  i2; break;
      case CMP_LE: res = i1 <= i2; break;
      case CMP_GE: res = i1 >= i2; break;
      default: assert(false);
    }
    state->result_value = alloc_bool(state, res);
    return;
  }
  
  Object *float_base = object_lookup(root, "float", NULL);
  Object *fobj2 = obj_instance_of(args_ptr[0], float_base);
  if (fobj2) {
    float v1 = ((IntObject*) iobj1)->value, v2 = ((FloatObject*) fobj2)->value;
    bool res;
    switch (cmp) {
      case CMP_EQ: res = v1 == v2; break;
      case CMP_LT: res = v1 <  v2; break;
      case CMP_GT: res = v1 >  v2; break;
      case CMP_LE: res = v1 <= v2; break;
      case CMP_GE: res = v1 >= v2; break;
      default: assert(false);
    }
    state->result_value = alloc_bool(state, res);
    return;
  }
  VM_ASSERT(false, "don't know how to compare int with object %p", args_ptr[0]);
}

static void int_eq_fn(VMState *state, Object *thisptr, Object *fn, Object **args_ptr, int args_len) {
  int_cmp_fn(state, thisptr, fn, args_ptr, args_len, CMP_EQ);
}

static void int_lt_fn(VMState *state, Object *thisptr, Object *fn, Object **args_ptr, int args_len) {
  int_cmp_fn(state, thisptr, fn, args_ptr, args_len, CMP_LT);
}

static void int_gt_fn(VMState *state, Object *thisptr, Object *fn, Object **args_ptr, int args_len) {
  int_cmp_fn(state, thisptr, fn, args_ptr, args_len, CMP_GT);
}

static void int_le_fn(VMState *state, Object *thisptr, Object *fn, Object **args_ptr, int args_len) {
  int_cmp_fn(state, thisptr, fn, args_ptr, args_len, CMP_LE);
}

static void int_ge_fn(VMState *state, Object *thisptr, Object *fn, Object **args_ptr, int args_len) {
  int_cmp_fn(state, thisptr, fn, args_ptr, args_len, CMP_GE);
}

static void float_cmp_fn(VMState *state, Object *thisptr, Object *fn, Object **args_ptr, int args_len, CompareOp cmp) {
  VM_ASSERT(args_len == 1, "wrong arity: expected 1, got %i", args_len);
  Object *root = state->root;
  
  Object *int_base = object_lookup(root, "int", NULL);
  Object *float_base = object_lookup(root, "float", NULL);
  Object
    *fobj1 = obj_instance_of(thisptr, float_base),
    *iobj2 = obj_instance_of(args_ptr[0], int_base),
    *fobj2 = obj_instance_of(args_ptr[0], float_base);
  VM_ASSERT(fobj1, "internal error: float compare function called on wrong type of object");
  if (fobj2 || iobj2) {
    float v1 = ((FloatObject*) fobj1)->value, v2;
    if (fobj2) v2 = ((FloatObject*) fobj2)->value;
    else v2 = ((IntObject*) iobj2)->value;
    bool res;
    switch (cmp) {
      case CMP_EQ: res = v1 == v2; break;
      case CMP_LT: res = v1 <  v2; break;
      case CMP_GT: res = v1 >  v2; break;
      case CMP_LE: res = v1 <= v2; break;
      case CMP_GE: res = v1 >= v2; break;
      default: assert(false);
    }
    state->result_value = alloc_bool(state, res);
    return;
  }
  VM_ASSERT(false, "don't know how to compare float with %p", args_ptr[0]);
}

static void float_eq_fn(VMState *state, Object *thisptr, Object *fn, Object **args_ptr, int args_len) {
  float_cmp_fn(state, thisptr, fn, args_ptr, args_len, CMP_EQ);
}

static void float_lt_fn(VMState *state, Object *thisptr, Object *fn, Object **args_ptr, int args_len) {
  float_cmp_fn(state, thisptr, fn, args_ptr, args_len, CMP_LT);
}

static void float_gt_fn(VMState *state, Object *thisptr, Object *fn, Object **args_ptr, int args_len) {
  float_cmp_fn(state, thisptr, fn, args_ptr, args_len, CMP_GT);
}

static void float_le_fn(VMState *state, Object *thisptr, Object *fn, Object **args_ptr, int args_len) {
  float_cmp_fn(state, thisptr, fn, args_ptr, args_len, CMP_LE);
}

static void float_ge_fn(VMState *state, Object *thisptr, Object *fn, Object **args_ptr, int args_len) {
  float_cmp_fn(state, thisptr, fn, args_ptr, args_len, CMP_GE);
}

static void closure_mark_fn(VMState *state, Object *obj) {
  Object *root = state->root;
  Object *closure_base = object_lookup(root, "closure", NULL);
  ClosureObject *clobj = (ClosureObject*) obj_instance_of(obj, closure_base);
  if (clobj) obj_mark(state, clobj->context);
}

static void array_mark_fn(VMState *state, Object *obj) {
  Object *root = state->root;
  Object *array_base = object_lookup(root, "array", NULL);
  ArrayObject *arr_obj = (ArrayObject*) obj_instance_of(obj, array_base);
  if (arr_obj) { // else it's obj == array_base
    for (int i = 0; i < arr_obj->length; ++i) {
      obj_mark(state, arr_obj->ptr[i]);
    }
  }
}

static void ptr_is_null_fn(VMState *state, Object *thisptr, Object *fn, Object **args_ptr, int args_len) {
  VM_ASSERT(args_len == 0, "wrong arity: expected 0, got %i", args_len);
  Object *root = state->root;
  Object *pointer_base = object_lookup(root, "pointer", NULL);
  PointerObject *ptr_obj = (PointerObject*) obj_instance_of(thisptr, pointer_base);
  state->result_value = alloc_bool(state, ptr_obj->ptr == NULL);
}

static void array_resize_fn(VMState *state, Object *thisptr, Object *fn, Object **args_ptr, int args_len) {
  VM_ASSERT(args_len == 1, "wrong arity: expected 1, got %i", args_len);
  Object *root = state->root;
  Object *int_base = object_lookup(root, "int", NULL);
  Object *array_base = object_lookup(root, "array", NULL);
  ArrayObject *arr_obj = (ArrayObject*) obj_instance_of(thisptr, array_base);
  IntObject *iarg = (IntObject*) obj_instance_of(args_ptr[0], int_base);
  VM_ASSERT(iarg, "parameter to resize function must be int");
  VM_ASSERT(arr_obj, "internal error: resize called on object that is not an array");
  int oldsize = arr_obj->length;
  int newsize = iarg->value;
  VM_ASSERT(newsize >= 0, "bad size: %i", newsize);
  arr_obj->ptr = realloc(arr_obj->ptr, sizeof(Object*) * newsize);
  memset(arr_obj->ptr + oldsize, 0, sizeof(Object*) * (newsize - oldsize));
  arr_obj->length = newsize;
  object_set(thisptr, "length", alloc_int(state, newsize));
  state->result_value = thisptr;
}

static void array_push_fn(VMState *state, Object *thisptr, Object *fn, Object **args_ptr, int args_len) {
  VM_ASSERT(args_len == 1, "wrong arity: expected 1, got %i", args_len);
  Object *root = state->root;
  Object *array_base = object_lookup(root, "array", NULL);
  ArrayObject *arr_obj = (ArrayObject*) obj_instance_of(thisptr, array_base);
  VM_ASSERT(arr_obj, "internal error: push called on object that is not an array");
  Object *value = args_ptr[0];
  arr_obj->ptr = realloc(arr_obj->ptr, sizeof(Object*) * ++arr_obj->length);
  arr_obj->ptr[arr_obj->length - 1] = value;
  object_set(thisptr, "length", alloc_int(state, arr_obj->length));
  state->result_value = thisptr;
}

static void array_pop_fn(VMState *state, Object *thisptr, Object *fn, Object **args_ptr, int args_len) {
  VM_ASSERT(args_len == 0, "wrong arity: expected 0, got %i", args_len);
  Object *root = state->root;
  Object *array_base = object_lookup(root, "array", NULL);
  ArrayObject *arr_obj = (ArrayObject*) obj_instance_of(thisptr, array_base);
  VM_ASSERT(arr_obj, "internal error: pop called on object that is not an array");
  Object *res = arr_obj->ptr[arr_obj->length - 1];
  arr_obj->ptr = realloc(arr_obj->ptr, sizeof(Object*) * --arr_obj->length);
  object_set(thisptr, "length", alloc_int(state, arr_obj->length));
  state->result_value = res;
}

static void array_index_fn(VMState *state, Object *thisptr, Object *fn, Object **args_ptr, int args_len) {
  VM_ASSERT(args_len == 1, "wrong arity: expected 1, got %i", args_len);
  Object *root = state->root;
  Object *int_base = object_lookup(root, "int", NULL);
  Object *array_base = object_lookup(root, "array", NULL);
  ArrayObject *arr_obj = (ArrayObject*) obj_instance_of(thisptr, array_base);
  IntObject *iarg = (IntObject*) obj_instance_of(args_ptr[0], int_base);
  if (!iarg) { state->result_value = NULL; return; }
  VM_ASSERT(arr_obj, "internal error: array '[]' called on object that is not an array");
  int index = iarg->value;
  VM_ASSERT(index >= 0 && index < arr_obj->length, "array index out of bounds!");
  state->result_value = arr_obj->ptr[index];
}

static void array_index_assign_fn(VMState *state, Object *thisptr, Object *fn, Object **args_ptr, int args_len) {
  VM_ASSERT(args_len == 2, "wrong arity: expected 2, got %i", args_len);
  Object *root = state->root;
  Object *int_base = object_lookup(root, "int", NULL);
  Object *array_base = object_lookup(root, "array", NULL);
  ArrayObject *arr_obj = (ArrayObject*) obj_instance_of(thisptr, array_base);
  IntObject *iarg = (IntObject*) obj_instance_of(args_ptr[0], int_base);
  VM_ASSERT(arr_obj, "internal error: array '[]=' called on object that is not an array");
  VM_ASSERT(iarg, "index of array '[]=' must be int");
  int index = iarg->value;
  VM_ASSERT(index >= 0 && index < arr_obj->length, "array index out of bounds!");
  Object *value = args_ptr[1];
  arr_obj->ptr[index] = value;
  state->result_value = NULL;
}

static void print_fn_recursive(VMState *state, Object *obj) {
  Object *root = state->root;
  Object *int_base = object_lookup(root, "int", NULL);
  Object *bool_base = object_lookup(root, "bool", NULL);
  Object *float_base = object_lookup(root, "float", NULL);
  Object *array_base = object_lookup(root, "array", NULL);
  Object *string_base = object_lookup(root, "string", NULL);
  Object
    *iobj = obj_instance_of(obj, int_base),
    *bobj = obj_instance_of(obj, bool_base),
    *fobj = obj_instance_of(obj, float_base),
    *sobj = obj_instance_of(obj, string_base),
    *aobj = obj_instance_of(obj, array_base);
  if (iobj) {
    printf("%i", ((IntObject*)iobj)->value);
    return;
  }
  if (bobj) {
    if (((BoolObject*)bobj)->value) printf("true");
    else printf("false");
    return;
  }
  if (fobj) {
    printf("%f", ((FloatObject*)fobj)->value);
    return;
  }
  if (sobj) {
    printf("%s", ((StringObject*)sobj)->value);
    return;
  }
  if (aobj) {
    printf("[ ");
    ArrayObject *a_obj = (ArrayObject*) aobj;
    for (int i = 0; i < a_obj->length; ++i) {
      Object *child = a_obj->ptr[i];
      if (i) printf(", ");
      print_fn_recursive(state, child);
      if (state->runstate == VM_ERRORED) return;
    }
    printf(" ]");
    return;
  }
  Object *toString_fn = object_lookup(obj, "toString", NULL);
  if (toString_fn) {
    Object *function_base = object_lookup(root, "function", NULL);
    Object *closure_base = object_lookup(root, "closure", NULL);
    FunctionObject *fn_toString = (FunctionObject*) obj_instance_of(toString_fn, function_base);
    ClosureObject *cl_toString = (ClosureObject*) obj_instance_of(toString_fn, closure_base);
    VM_ASSERT(fn_toString || cl_toString, "'toString' property is neither function nor closure");
    
    VMState substate = {0};
    substate.parent = state;
    substate.root = state->root;
    substate.gcstate = state->gcstate;
    substate.profstate = state->profstate;
    
    if (fn_toString) fn_toString->fn_ptr(&substate, obj, toString_fn, NULL, 0);
    else cl_toString->base.fn_ptr(&substate, obj, toString_fn, NULL, 0);
    
    vm_run(&substate);
    VM_ASSERT(substate.runstate != VM_ERRORED, "toString failure: %s\n", substate.error);
    
    Object *str = substate.result_value;
    if (str != NULL) {
      print_fn_recursive(state, str);
      return;
    }
  }
  printf("[object %p", (void*) obj);
  HashTable *tbl = &obj->tbl;
  bool first = true;
  for (int i = 0; i < tbl->entries_num; ++i) {
    TableEntry *entry = &tbl->entries_ptr[i];
    if (entry->name_ptr) {
      if (first) { first = false; printf(" | "); }
      else printf(", ");
      printf("'%.*s': ", (int) entry->name_len, entry->name_ptr);
      print_fn_recursive(state, entry->value);
    }
  }
  printf("]");
  // vm_error(state, "don't know how to print %p", obj);
  return;
}

static void print_fn(VMState *state, Object *thisptr, Object *fn, Object **args_ptr, int args_len) {
  for (int i = 0; i < args_len; ++i) {
    Object *arg = args_ptr[i];
    print_fn_recursive(state, arg);
    if (state->runstate == VM_ERRORED) return;
  }
  printf("\n");
  state->result_value = NULL;
}

static void keys_fn(VMState *state, Object *thisptr, Object *fn, Object **args_ptr, int args_len) {
  VM_ASSERT(args_len == 1, "wrong arity: expected 1, got %i", args_len);
  gc_disable(state);
  int res_len = 0;
  Object *obj = args_ptr[0];
  HashTable *tbl = &obj->tbl;
  for (int i = 0; i < tbl->entries_num; ++i) {
    TableEntry *entry = &tbl->entries_ptr[i];
    if (entry->name_ptr) res_len ++;
  }
  Object **res_ptr = malloc(sizeof(Object*) * res_len);
  int k = 0;
  for (int i = 0; i < tbl->entries_num; ++i) {
    TableEntry *entry = &tbl->entries_ptr[i];
    if (entry->name_ptr) {
      res_ptr[k++] = alloc_string(state, entry->name_ptr);
    }
  }
  state->result_value = alloc_array(state, res_ptr, res_len);
  gc_enable(state);
}

static Object *xml_to_object(VMState *state, xmlNode *element, Object *text_node, Object *element_node) {
  Object *res;
  if (element->type == 1) {
    res = alloc_object(state, element_node);
    xmlNode *child = element->children;
    int children_len = 0;
    for (; child; child = child->next) children_len ++;
    Object **children_ptr = malloc(sizeof(Object*) * children_len);
    int i = 0;
    for (child = element->children; child; child = child->next) {
      children_ptr[i++] = xml_to_object(state, child, text_node, element_node);
    }
    Object *attr = alloc_object(state, NULL);
    xmlAttr *xml_attr = element->properties;
    for (; xml_attr; xml_attr = xml_attr->next) {
      assert(xml_attr->type == 2); // attribute
      assert(xml_attr->children && xml_attr->children->type == 3); // text (TODO handle other types?)
      // TODO reference source document
      // TODO string source reference handling
      // clone xml_attr->name
      int name_size = strlen((char*) xml_attr->name);
      char *name2 = malloc(name_size + 1);
      strncpy(name2, (char*) xml_attr->name, name_size + 1);
      // printf("alloc_string(%lu)\n", strlen((char*) xml_attr->children->content));
      object_set(attr, name2, alloc_string(state, (char*) xml_attr->children->content));
    }
    // printf("alloc_string(%lu)\n", strlen((char*) element->name));
    object_set(res, "nodeName", alloc_string(state, (char*) element->name));
    object_set(res, "attr", attr);
    object_set(res, "children", alloc_array(state, children_ptr, children_len));
  } else if (element->type == 3) {
    res = alloc_object(state, text_node);
    // printf("alloc_string(%lu)\n", strlen((char*) element->content));
    object_set(res, "value", alloc_string(state, (char*) element->content));
  } else assert(false);
  return res;
}

static void xml_load_fn(VMState *state, Object *thisptr, Object *fn, Object **args_ptr, int args_len) {
  VM_ASSERT(args_len == 1, "wrong arity: expected 1, got %i", args_len);
  Object *root = state->root;
  Object *string_base = object_lookup(root, "string", NULL);
  Object *node_base = object_lookup(object_lookup(root, "xml", NULL), "node", NULL);
  
  StringObject *str_obj = (StringObject*) obj_instance_of(args_ptr[0], string_base);
  VM_ASSERT(str_obj, "parameter to xml.load must be string");
  char *file = str_obj->value;
  
  LIBXML_TEST_VERSION
  
  xmlDoc *doc = xmlReadFile(file, NULL, 0);
  VM_ASSERT(doc != NULL, "cannot read xml file");
  
  xmlNode *root_element = xmlDocGetRootElement(doc);
  
  gc_disable(state);
  
  Object *text_node = alloc_object(state, node_base);
  object_set(text_node, "nodeName", alloc_string_foreign(state, ""));
  object_set(text_node, "nodeType", alloc_int(state, 3));
  object_set(text_node, "attr", alloc_object(state, NULL));
  object_set(text_node, "children", alloc_array(state, NULL, 0));
  
  Object *element_node = alloc_object(state, node_base);
  object_set(element_node, "nodeType", alloc_int(state, 1));
  object_set(element_node, "value", NULL);
  
  state->result_value = xml_to_object(state, root_element, text_node, element_node);
  gc_enable(state);
  // letting it leak is actually _saving_ us memory
  // at least until we can properly track the source resource in string allocation
  xmlFreeDoc(doc);
  xmlCleanupParser();
}

static bool xml_node_check_pred(VMState *state, Object *node, Object *pred,
                                Object *function_base, Object *closure_base, Object *bool_base)
{
  FunctionObject *fn_pred = (FunctionObject*) obj_instance_of(pred, function_base);
  ClosureObject *cl_pred = (ClosureObject*) obj_instance_of(pred, closure_base);
  VM_ASSERT(fn_pred || cl_pred, "predicate is neither function nor closure") false;
  
  VMState substate = {0};
  substate.parent = state;
  substate.root = state->root;
  substate.gcstate = state->gcstate;
  substate.profstate = state->profstate;
  
  if (fn_pred) fn_pred->fn_ptr(&substate, node, pred, &node, 1);
  else cl_pred->base.fn_ptr(&substate, node, pred, &node, 1);
  
  vm_run(&substate);
  VM_ASSERT(substate.runstate != VM_ERRORED, "toString failure: %s\n", substate.error) false;
  
  // TODO truthy()
  BoolObject *b_res = (BoolObject*) obj_instance_of(substate.result_value, bool_base);
  VM_ASSERT(b_res, "predicate must return bool") false;
  
  return b_res->value;
}

static void xml_node_find_recurse(VMState *state, Object *node, Object *pred, Object ***array_p_p, int *array_l_p,
                                  Object *function_base, Object *closure_base, Object *bool_base, Object *array_base)
{
  bool res = xml_node_check_pred(state, node, pred, function_base, closure_base, bool_base);
  if (state->runstate == VM_ERRORED) return;
  if (res) {
    (*array_p_p) = realloc((void*) *array_p_p, sizeof(Object*) * ++(*array_l_p));
    (*array_p_p)[(*array_l_p) - 1] = node;
  }
  
  Object *children_obj = object_lookup(node, "children", NULL);
  VM_ASSERT(children_obj, "missing 'children' property in node");
  ArrayObject *children_aobj = (ArrayObject*) obj_instance_of(children_obj, array_base);
  VM_ASSERT(children_aobj, "'children' property in node is not an array");
  for (int i = 0; i < children_aobj->length; ++i) {
    Object *child = children_aobj->ptr[i];
    xml_node_find_recurse(state, child, pred, array_p_p, array_l_p,
                          function_base, closure_base, bool_base, array_base);
    if (state->runstate == VM_ERRORED) return;
  }
}

static void xml_node_find_array_fn(VMState *state, Object *thisptr, Object *fn, Object **args_ptr, int args_len) {
  VM_ASSERT(args_len == 1, "wrong arity: expected 1, got %i", args_len);
  Object *root = state->root;
  Object *bool_base = object_lookup(root, "bool", NULL);
  Object *array_base = object_lookup(root, "array", NULL);
  Object *closure_base = object_lookup(root, "closure", NULL);
  Object *function_base = object_lookup(root, "function", NULL);
  
  Object **array_ptr = NULL; int array_length = 0;
  xml_node_find_recurse(state, thisptr, args_ptr[0], &array_ptr, &array_length,
                        function_base, closure_base, bool_base, array_base);
  state->result_value = alloc_array(state, array_ptr, array_length);
}

Object *create_root(VMState *state) {
  Object *root = alloc_object(state, NULL);
  
  state->root = root;
  state->stack_ptr[state->stack_len - 1].context = root;
  
  GCRootSet pin_root;
  gc_add_roots(state, &root, 1, &pin_root);
  
  Object *function_obj = alloc_object(state, NULL);
  function_obj->flags |= OBJ_NOINHERIT;
  object_set(root, "function", function_obj);
  
  Object *int_obj = alloc_object(state, NULL);
  int_obj->flags |= OBJ_NOINHERIT;
  object_set(root, "int", int_obj);
  object_set(int_obj, "+" , alloc_fn(state, int_add_fn));
  object_set(int_obj, "-" , alloc_fn(state, int_sub_fn));
  object_set(int_obj, "*" , alloc_fn(state, int_mul_fn));
  object_set(int_obj, "/" , alloc_fn(state, int_div_fn));
  object_set(int_obj, "==", alloc_fn(state, int_eq_fn));
  object_set(int_obj, "<" , alloc_fn(state, int_lt_fn));
  object_set(int_obj, ">" , alloc_fn(state, int_gt_fn));
  object_set(int_obj, "<=", alloc_fn(state, int_le_fn));
  object_set(int_obj, ">=", alloc_fn(state, int_ge_fn));
  object_set(int_obj, "parse" , alloc_fn(state, int_parse_fn));
  
  Object *float_obj = alloc_object(state, NULL);
  float_obj->flags |= OBJ_NOINHERIT;
  object_set(root, "float", float_obj);
  object_set(float_obj, "+" , alloc_fn(state, float_add_fn));
  object_set(float_obj, "-" , alloc_fn(state, float_sub_fn));
  object_set(float_obj, "*" , alloc_fn(state, float_mul_fn));
  object_set(float_obj, "/" , alloc_fn(state, float_div_fn));
  object_set(float_obj, "==", alloc_fn(state, float_eq_fn));
  object_set(float_obj, "<" , alloc_fn(state, float_lt_fn));
  object_set(float_obj, ">" , alloc_fn(state, float_gt_fn));
  object_set(float_obj, "<=", alloc_fn(state, float_le_fn));
  object_set(float_obj, ">=", alloc_fn(state, float_ge_fn));
  
  Object *closure_obj = alloc_object(state, NULL);
  closure_obj->flags |= OBJ_NOINHERIT;
  closure_obj->mark_fn = closure_mark_fn;
  object_set(root, "closure", closure_obj);
  
  Object *bool_obj = alloc_object(state, NULL);
  bool_obj->flags |= OBJ_NOINHERIT;
  object_set(root, "bool", bool_obj);
  object_set(bool_obj, "!", alloc_fn(state, bool_not_fn));
  object_set(root, "true", alloc_bool(state, true));
  object_set(root, "false", alloc_bool(state, false));
  
  object_set(root, "null", NULL);
  
  Object *string_obj = alloc_object(state, NULL);
  string_obj->flags |= OBJ_NOINHERIT;
  object_set(root, "string", string_obj);
  object_set(string_obj, "+", alloc_fn(state, string_add_fn));
  object_set(string_obj, "==", alloc_fn(state, string_eq_fn));
  
  Object *array_obj = alloc_object(state, NULL);
  array_obj->flags |= OBJ_NOINHERIT;
  array_obj->mark_fn = array_mark_fn;
  object_set(root, "array", array_obj);
  object_set(array_obj, "resize", alloc_fn(state, array_resize_fn));
  object_set(array_obj, "push", alloc_fn(state, array_push_fn));
  object_set(array_obj, "pop", alloc_fn(state, array_pop_fn));
  object_set(array_obj, "[]", alloc_fn(state, array_index_fn));
  object_set(array_obj, "[]=", alloc_fn(state, array_index_assign_fn));
  
  Object *ptr_obj = alloc_object(state, NULL);
  ptr_obj->flags |= OBJ_NOINHERIT;
  object_set(root, "pointer", ptr_obj);
  object_set(ptr_obj, "null", alloc_fn(state, ptr_is_null_fn));
  
  object_set(root, "print", alloc_fn(state, print_fn));
  object_set(root, "keys", alloc_fn(state, keys_fn));
  
  Object *xml_obj = alloc_object(state, NULL);
  object_set(xml_obj, "load", alloc_fn(state, xml_load_fn));
  Object *node_obj = alloc_object(state, NULL);
  object_set(xml_obj, "node", node_obj);
  object_set(node_obj, "find_array", alloc_fn(state, xml_node_find_array_fn));
  xml_obj->flags |= OBJ_IMMUTABLE;
  object_set(root, "xml", xml_obj);
  
  ffi_setup_root(state, root);
  
  root->flags |= OBJ_IMMUTABLE;
  gc_remove_roots(state, &pin_root);
  
  return root;
}