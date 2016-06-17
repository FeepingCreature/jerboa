#include "vm/runtime.h"

#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>

#include "vm/call.h"
#include "vm/ffi.h"
#include "gc.h"

static void asprintf(char **outp, char *fmt, ...) {
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
  Object *root = state->stack_ptr[state->stack_len - 1].context;
  while (root->parent) root = root->parent;
  
  state->result_value = alloc_bool(state, ! ((BoolObject*) thisptr)->value);
}

typedef enum {
  MATH_ADD,
  MATH_SUB,
  MATH_MUL,
  MATH_DIV
} MathOp;

static void int_math_fn(VMState *state, Object *thisptr, Object *fn, Object **args_ptr, int args_len, MathOp mop) {
  VM_ASSERT(args_len == 1, "wrong arity: expected 1, got %i", args_len);
  Object *root = state->stack_ptr[state->stack_len - 1].context;
  while (root->parent) root = root->parent;
  
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

static void float_math_fn(VMState *state, Object *thisptr, Object *fn, Object **args_ptr, int args_len, MathOp mop) {
  VM_ASSERT(args_len == 1, "wrong arity: expected 1, got %i", args_len);
  Object *root = state->stack_ptr[state->stack_len - 1].context;
  while (root->parent) root = root->parent;
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
  Object *root = state->stack_ptr[state->stack_len - 1].context;
  while (root->parent) root = root->parent;
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
  if (sobj2) asprintf(&str2, "%s", ((StringObject*) sobj2)->value);
  else if (fobj2) asprintf(&str2, "%f", ((FloatObject*) fobj2)->value);
  else if (iobj2) asprintf(&str2, "%i", ((IntObject*) iobj2)->value);
  else if (bobj2) if (((BoolObject*)bobj2)->value) asprintf(&str2, "%s", "true"); else asprintf(&str2, "%s", "false");
  else VM_ASSERT(false, "don't know how to format object: %p", args_ptr[0]);
  char *str3;
  asprintf(&str3, "%s%s", str1, str2);
  free(str2);
  state->result_value = alloc_string(state, str3);
  free(str3);
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
  Object *root = state->stack_ptr[state->stack_len - 1].context;
  while (root->parent) root = root->parent;
  
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
  Object *root = state->stack_ptr[state->stack_len - 1].context;
  while (root->parent) root = root->parent;
  
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
  Object *root = state->stack_ptr[state->stack_len - 1].context;
  while (root->parent) root = root->parent;
  Object *closure_base = object_lookup(root, "closure", NULL);
  ClosureObject *clobj = (ClosureObject*) obj_instance_of(obj, closure_base);
  if (clobj) obj_mark(state, clobj->context);
}

static void array_mark_fn(VMState *state, Object *obj) {
  Object *root = state->stack_ptr[state->stack_len - 1].context;
  while (root->parent) root = root->parent;
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
  Object *root = state->stack_ptr[state->stack_len - 1].context;
  while (root->parent) root = root->parent;
  Object *pointer_base = object_lookup(root, "pointer", NULL);
  PointerObject *ptr_obj = (PointerObject*) obj_instance_of(thisptr, pointer_base);
  state->result_value = alloc_bool(state, ptr_obj->ptr == NULL);
}

static void array_resize_fn(VMState *state, Object *thisptr, Object *fn, Object **args_ptr, int args_len) {
  VM_ASSERT(args_len == 1, "wrong arity: expected 1, got %i", args_len);
  Object *root = state->stack_ptr[state->stack_len - 1].context;
  while (root->parent) root = root->parent;
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
  Object *root = state->stack_ptr[state->stack_len - 1].context;
  while (root->parent) root = root->parent;
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
  Object *root = state->stack_ptr[state->stack_len - 1].context;
  while (root->parent) root = root->parent;
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
  Object *root = state->stack_ptr[state->stack_len - 1].context;
  while (root->parent) root = root->parent;
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
  Object *root = state->stack_ptr[state->stack_len - 1].context;
  while (root->parent) root = root->parent;
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

static void print_fn(VMState *state, Object *thisptr, Object *fn, Object **args_ptr, int args_len) {
  Object *root = state->stack_ptr[state->stack_len - 1].context;
  while (root->parent) root = root->parent;
  Object *int_base = object_lookup(root, "int", NULL);
  Object *bool_base = object_lookup(root, "bool", NULL);
  Object *float_base = object_lookup(root, "float", NULL);
  Object *string_base = object_lookup(root, "string", NULL);
  
  for (int i = 0; i < args_len; ++i) {
    Object *arg = args_ptr[i];
    Object
      *iarg = obj_instance_of(arg, int_base),
      *barg = obj_instance_of(arg, bool_base),
      *farg = obj_instance_of(arg, float_base),
      *sarg = obj_instance_of(arg, string_base);
    if (iarg) {
      printf("%i", ((IntObject*)iarg)->value);
      continue;
    }
    if (barg) {
      if (((BoolObject*)barg)->value) printf("true");
      else printf("false");
      continue;
    }
    if (farg) {
      printf("%f", ((FloatObject*)farg)->value);
      continue;
    }
    if (sarg) {
      printf("%s", ((StringObject*)sarg)->value);
      continue;
    }
    vm_error(state, "don't know how to print %p", arg);
    return;
  }
  printf("\n");
  state->result_value = NULL;
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
  object_set(root, "closure", closure_obj);
  
  Object *closure_gc = alloc_custom_gc(state);
  ((CustomGCObject*) closure_gc)->mark_fn = closure_mark_fn;
  object_set(closure_obj, "gc", closure_gc); // note: gc must be first entry in object
  
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
  
  Object *array_obj = alloc_object(state, NULL);
  array_obj->flags |= OBJ_NOINHERIT;
  object_set(root, "array", array_obj);
  Object *array_gc =  alloc_custom_gc(state);
  ((CustomGCObject*) array_gc)->mark_fn = array_mark_fn;
  object_set(array_obj, "gc", array_gc); // note: gc must be first entry in object
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
  
  ffi_setup_root(state, root);
  
  root->flags |= OBJ_IMMUTABLE;
  gc_remove_roots(state, &pin_root);
  
  return root;
}
