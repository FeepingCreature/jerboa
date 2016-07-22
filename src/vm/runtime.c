#include "vm/runtime.h"

#include <math.h>
#include <stdio.h>
#include <errno.h>
#include <stdint.h>

#include "vm/call.h"
#include "vm/ffi.h"
#include "gc.h"
#include "print.h"

#include <libxml/parser.h>
#include <libxml/tree.h>

static void fn_apply_fn(VMState *state, Object *thisptr, Object *fn, Object **args_ptr, int args_len) {
  VM_ASSERT(args_len == 1, "wrong arity: expected 1, got %i", args_len);
  Object *array_base = state->shared->vcache.array_base;
  ArrayObject *args_array = (ArrayObject*) obj_instance_of(args_ptr[0], array_base);
  VM_ASSERT(args_array, "argument to apply() must be array!");
  Object *call_fn = thisptr;
  // passthrough call to actual function
  // note: may set its own errors
  setup_call(state, NULL, call_fn, args_array->ptr, args_array->length);
}

// TODO add "IS_TRUTHY" instr so we can promote, say, null, to bool before calling this
static void bool_not_fn(VMState *state, Object *thisptr, Object *fn, Object **args_ptr, int args_len) {
  VM_ASSERT(args_len == 0, "wrong arity: expected 0, got %i", args_len);
  Object *bool_base = state->shared->vcache.bool_base;
  VM_ASSERT(thisptr->parent == bool_base, "internal error: bool negation called on wrong type of object");
  
  state->result_value = alloc_bool(state, !thisptr->bool_value);
}

static void bool_eq_fn(VMState *state, Object *thisptr, Object *fn, Object **args_ptr, int args_len) {
  VM_ASSERT(args_len == 1, "wrong arity: expected 1, got %i", args_len);
  VM_ASSERT(args_ptr[0], "don't know how to compare bool with null");
  
  Object
    *bool_base = state->shared->vcache.bool_base,
    *obj1 = thisptr,
    *obj2 = args_ptr[0];
  
  VM_ASSERT(obj1->parent == bool_base, "internal error: bool compare function called on wrong type of object");
  VM_ASSERT(obj2->parent == bool_base, "internal error: bool compare function called on wrong type of object");
  state->result_value = alloc_bool(state, obj1->bool_value == obj2->bool_value);
}

typedef enum {
  MATH_ADD,
  MATH_SUB,
  MATH_MUL,
  MATH_DIV,
  MATH_BIT_OR,
  MATH_BIT_AND
} MathOp;

static void int_math_fn(VMState *state, Object *thisptr, Object *fn, Object **args_ptr, int args_len, MathOp mop) {
  VM_ASSERT(args_len == 1, "wrong arity: expected 1, got %i", args_len);
  VM_ASSERT(args_ptr[0], "don't know how to compute with null");
  
  Object *int_base = state->shared->vcache.int_base;
  Object *float_base = state->shared->vcache.float_base;
  
  VM_ASSERT(thisptr->parent == int_base, "internal error: int math function called on wrong type of object"); // otherwise how are we called on it??
  Object *obj2 = args_ptr[0];
  
  if (obj2->parent == int_base) {
    int i1 = thisptr->int_value, i2 = obj2->int_value;
    int res;
    switch (mop) {
      case MATH_ADD: res = i1 + i2; break;
      case MATH_SUB: res = i1 - i2; break;
      case MATH_MUL: res = i1 * i2; break;
      case MATH_DIV:
        VM_ASSERT(i2 != 0, "division by zero");
        res = i1 / i2;
        break;
      case MATH_BIT_OR: res = i1 | i2; break;
      case MATH_BIT_AND: res = i1 & i2; break;
      default: abort();
    }
    state->result_value = alloc_int(state, res);
    return;
  }
  
  if (obj2->parent == float_base) {
    float v1 = thisptr->int_value, v2 = obj2->float_value;
    float res;
    switch (mop) {
      case MATH_ADD: res = v1 + v2; break;
      case MATH_SUB: res = v1 - v2; break;
      case MATH_MUL: res = v1 * v2; break;
      case MATH_DIV:
        VM_ASSERT(v2 != 0.0f, "float division by zero");
        res = v1 / v2;
        break;
      case MATH_BIT_OR: case MATH_BIT_AND:
        VM_ASSERT(false, "bit math with float operands is not supported");
      default: abort();
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

static void int_bit_or_fn(VMState *state, Object *thisptr, Object *fn, Object **args_ptr, int args_len) {
  int_math_fn(state, thisptr, fn, args_ptr, args_len, MATH_BIT_OR);
}

static void int_bit_and_fn(VMState *state, Object *thisptr, Object *fn, Object **args_ptr, int args_len) {
  int_math_fn(state, thisptr, fn, args_ptr, args_len, MATH_BIT_AND);
}

static void int_parse_fn(VMState *state, Object *thisptr, Object *fn, Object **args_ptr, int args_len) {
  VM_ASSERT(args_len == 1, "wrong arity: expected 1, got %i", args_len);
  Object *string_base = state->shared->vcache.string_base;
  
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
  VM_ASSERT(args_ptr[0], "don't know how to compute with null");
  
  Object *int_base = state->shared->vcache.int_base;
  Object *float_base = state->shared->vcache.float_base;
  
  Object *obj1 = thisptr;
  Object *obj2 = args_ptr[0];
  VM_ASSERT(obj1->parent == float_base, "internal error: float math function called on wrong type of object");
  
  float v1 = obj1->float_value, v2;
  if (obj2->parent == float_base) v2 = obj2->float_value;
  else if (obj2->parent == int_base) v2 = obj2->int_value;
  else { vm_error(state, "don't know how to perform float math with %p", args_ptr[0]); return; }
  
  float res;
  switch (mop) {
    case MATH_ADD: res = v1 + v2; break;
    case MATH_SUB: res = v1 - v2; break;
    case MATH_MUL: res = v1 * v2; break;
    case MATH_DIV: res = v1 / v2; break;
    case MATH_BIT_OR:
    case MATH_BIT_AND: vm_error(state, "bitops are undefined for float");
    default: abort();
  }
  state->result_value = alloc_float(state, res);
  return;
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
  VM_ASSERT(args_ptr[0], "don't know how to add null to string");
  
  Object
    *sobj1 = obj_instance_of(thisptr, state->shared->vcache.string_base),
    *obj2 = args_ptr[0],
    *int_base = state->shared->vcache.int_base,
    *bool_base = state->shared->vcache.bool_base,
    *float_base = state->shared->vcache.float_base,
    *sobj2 = obj_instance_of(obj2, state->shared->vcache.string_base);
  VM_ASSERT(sobj1, "internal error: string concat function called on wrong type of object");
  
  char *str1 = ((StringObject*) sobj1)->value, *str2;
  if (sobj2) str2 = my_asprintf("%s", ((StringObject*) sobj2)->value);
  else if (obj2->parent == float_base) str2 = my_asprintf("%f", obj2->float_value);
  else if (obj2->parent == int_base) str2 = my_asprintf("%i", obj2->int_value);
  else if (obj2->parent == bool_base) if (obj2->bool_value) str2 = my_asprintf("%s", "true"); else str2 = my_asprintf("%s", "false");
  else VM_ASSERT(false, "don't know how to format object: %p", args_ptr[0]);
  char *str3 = my_asprintf("%s%s", str1, str2);
  free(str2);
  state->result_value = alloc_string(state, str3, strlen(str3));
  free(str3);
}

static void string_eq_fn(VMState *state, Object *thisptr, Object *fn, Object **args_ptr, int args_len) {
  VM_ASSERT(args_len == 1, "wrong arity: expected 1, got %i", args_len);
  Object *string_base = state->shared->vcache.string_base;
  
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

static void string_startswith_fn(VMState *state, Object *thisptr, Object *fn, Object **args_ptr, int args_len) {
  VM_ASSERT(args_len == 1, "wrong arity: expected 1, got %i", args_len);
  Object *string_base = state->shared->vcache.string_base;
  
  Object
    *sobj1 = obj_instance_of(thisptr, string_base),
    *sobj2 = obj_instance_of(args_ptr[0], string_base);
  VM_ASSERT(sobj1, "internal error: string.startsWith() called on wrong type of object");
  VM_ASSERT(sobj2, "string.startsWith() expects string as parameter");
  
  char
    *str1 = ((StringObject*) sobj1)->value,
    *str2 = ((StringObject*) sobj2)->value;
  int len1 = strlen(str1);
  int len2 = strlen(str2);
  int res;
  if (len2 > len1) res = -1;
  else res = strncmp(str1, str2, len2);
  if (res == 0) {
    state->result_value = alloc_string(state, str1 + len2, strlen(str1) - len2);
  } else {
    state->result_value = NULL;
  }
}

static void string_endswith_fn(VMState *state, Object *thisptr, Object *fn, Object **args_ptr, int args_len) {
  VM_ASSERT(args_len == 1, "wrong arity: expected 1, got %i", args_len);
  Object *string_base = state->shared->vcache.string_base;
  
  Object
    *sobj1 = obj_instance_of(thisptr, string_base),
    *sobj2 = obj_instance_of(args_ptr[0], string_base);
  VM_ASSERT(sobj1, "internal error: string.endsWith() called on wrong type of object");
  VM_ASSERT(sobj2, "string.endsWith() expects string as parameter");
  
  char
    *str1 = ((StringObject*) sobj1)->value,
    *str2 = ((StringObject*) sobj2)->value;
  int len1 = strlen(str1);
  int len2 = strlen(str2);
  int res;
  if (len2 > len1) res = -1;
  else res = strncmp(str1 + len1 - len2, str2, len2);
  if (res == 0) {
    state->result_value = alloc_string(state, str1, strlen(str1) - len2);
  } else {
    state->result_value = NULL;
  }
}

static void string_slice_fn(VMState *state, Object *thisptr, Object *fn, Object **args_ptr, int args_len) {
  VM_ASSERT(args_len == 1 || args_len == 2, "wrong arity: expected 1 or 2, got %i", args_len);
  Object *string_base = state->shared->vcache.string_base;
  Object *int_base = state->shared->vcache.int_base;
  
  StringObject *sobj = (StringObject*) obj_instance_of(thisptr, string_base);
  VM_ASSERT(sobj, "internal error: string.slice() called on wrong type of object");
  
  char *str = sobj->value;
  int len = utf8_strlen(str);
  int from = 0, to = len;
  if (args_len == 1) {
    Object *arg1 = args_ptr[0];
    VM_ASSERT(arg1->parent == int_base, "string.slice() expected int");
    from = arg1->int_value;
  } else {
    Object *arg1 = args_ptr[0], *arg2 = args_ptr[1];
    VM_ASSERT(arg1->parent == int_base, "string.slice() expected int as first parameter");
    VM_ASSERT(arg2->parent == int_base, "string.slice() expected int as second parameter");
    from = arg1->int_value;
    to = arg2->int_value;
  }
  VM_ASSERT(from >= 0 && from <= len, "string.slice() start must lie inside string");
  VM_ASSERT(to >= 0 && to <= len, "string.slice() end must lie inside string");
  VM_ASSERT(from <= to, "string.slice() start must lie before end");
  
  const char *start = str;
  const char *error = NULL;
  utf8_step(&start, from, &error);
  VM_ASSERT(!error, error); // lol
  
  const char *end = start;
  error = NULL;
  utf8_step(&end, to - from, &error);
  VM_ASSERT(!error, error);
  
  state->result_value = alloc_string(state, start, end - start);
}

static void string_find_fn(VMState *state, Object *thisptr, Object *fn, Object **args_ptr, int args_len) {
  VM_ASSERT(args_len == 1, "wrong arity: expected 1, got %i", args_len);
  Object *string_base = state->shared->vcache.string_base;
  
  StringObject *sobj = (StringObject*) obj_instance_of(thisptr, string_base);
  VM_ASSERT(sobj, "internal error: string.find() called on wrong type of object");
  StringObject *sobj2 = (StringObject*) obj_instance_of(args_ptr[0], string_base);
  VM_ASSERT(sobj2, "internal error: string.find() expects string");
  
  char *str = sobj->value;
  int len = strlen(str);
  char *match = sobj2->value;
  int matchlen = strlen(match);
  if (matchlen == 0) {
    state->result_value = alloc_int(state, 0);
    return;
  }
  
  char *pos = memmem(str, len, match, matchlen);
  if (pos == NULL) {
    state->result_value = alloc_int(state, -1);
    return;
  }
  
  int pos_utf8 = utf8_strnlen(str, pos - str);
  state->result_value = alloc_int(state, pos_utf8);
}

static void string_byte_len_fn(VMState *state, Object *thisptr, Object *fn, Object **args_ptr, int args_len) {
  VM_ASSERT(args_len == 0, "wrong arity: expected 0, got %i", args_len);
  Object *string_base = state->shared->vcache.string_base;
  
  Object *sobj = obj_instance_of(thisptr, string_base);
  VM_ASSERT(sobj, "internal error: string.endsWith() called on wrong type of object");
  
  char *str = ((StringObject*) sobj)->value;
  state->result_value = alloc_int(state, strlen(str));
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
  VM_ASSERT(args_ptr[0], "don't know how to compare int with null");
  
  Object
    *int_base = state->shared->vcache.int_base,
    *float_base = state->shared->vcache.float_base,
    *obj1 = thisptr,
    *obj2 = args_ptr[0];
  
  VM_ASSERT(obj1->parent == int_base, "internal error: int compare function called on wrong type of object");
  if (obj2->parent == int_base) {
    int i1 = obj1->int_value, i2 = obj2->int_value;
    bool res;
    switch (cmp) {
      case CMP_EQ: res = i1 == i2; break;
      case CMP_LT: res = i1 <  i2; break;
      case CMP_GT: res = i1 >  i2; break;
      case CMP_LE: res = i1 <= i2; break;
      case CMP_GE: res = i1 >= i2; break;
      default: abort();
    }
    state->result_value = alloc_bool(state, res);
    return;
  }
  
  if (obj2->parent == float_base) {
    float v1 = obj1->int_value, v2 = obj2->float_value;
    bool res;
    switch (cmp) {
      case CMP_EQ: res = v1 == v2; break;
      case CMP_LT: res = v1 <  v2; break;
      case CMP_GT: res = v1 >  v2; break;
      case CMP_LE: res = v1 <= v2; break;
      case CMP_GE: res = v1 >= v2; break;
      default: abort();
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
  VM_ASSERT(args_ptr[0], "don't know how to compare float with null");
  
  Object
    *int_base = state->shared->vcache.int_base,
    *float_base = state->shared->vcache.float_base,
    *obj1 = thisptr,
    *obj2 = args_ptr[0];
  VM_ASSERT(obj1->parent == float_base, "internal error: float compare function called on wrong type of object");
  
  float v1 = obj1->float_value, v2;
  if (obj2->parent == float_base) v2 = obj2->float_value;
  else if (obj2->parent == int_base) v2 = obj2->int_value;
  else { vm_error(state, "don't know how to compare float with %p", obj2); return; }
  
  bool res;
  switch (cmp) {
    case CMP_EQ: res = v1 == v2; break;
    case CMP_LT: res = v1 <  v2; break;
    case CMP_GT: res = v1 >  v2; break;
    case CMP_LE: res = v1 <= v2; break;
    case CMP_GE: res = v1 >= v2; break;
    default: abort();
  }
  state->result_value = alloc_bool(state, res);
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

static void float_toint_fn(VMState *state, Object *thisptr, Object *fn, Object **args_ptr, int args_len) {
  VM_ASSERT(args_len == 0, "wrong arity: expected 0, got %i", args_len);
  Object *float_base = state->shared->vcache.float_base;
  
  VM_ASSERT(thisptr && thisptr->parent == float_base, "float.toInt called on wrong type of object");
  state->result_value = alloc_int(state, (int) thisptr->float_value);
}

static void ptr_is_null_fn(VMState *state, Object *thisptr, Object *fn, Object **args_ptr, int args_len) {
  VM_ASSERT(args_len == 0, "wrong arity: expected 0, got %i", args_len);
  Object *pointer_base = state->shared->vcache.pointer_base;
  VM_ASSERT(thisptr->parent == pointer_base, "internal error");
  PointerObject *ptr_obj = (PointerObject*) thisptr;
  state->result_value = alloc_bool(state, ptr_obj->ptr == NULL);
}

static void array_resize_fn(VMState *state, Object *thisptr, Object *fn, Object **args_ptr, int args_len) {
  VM_ASSERT(args_len == 1, "wrong arity: expected 1, got %i", args_len);
  Object *int_base = state->shared->vcache.int_base;
  Object *array_base = state->shared->vcache.array_base;
  ArrayObject *arr_obj = (ArrayObject*) obj_instance_of(thisptr, array_base);
  Object *arg = args_ptr[0];
  VM_ASSERT(arg->parent == int_base, "parameter to resize function must be int");
  VM_ASSERT(arr_obj, "internal error: resize called on object that is not an array");
  int oldsize = arr_obj->length;
  int newsize = arg->int_value;
  VM_ASSERT(newsize >= 0, "bad size: %i", newsize);
  arr_obj->ptr = realloc(arr_obj->ptr, sizeof(Object*) * newsize);
  memset(arr_obj->ptr + oldsize, 0, sizeof(Object*) * (newsize - oldsize));
  arr_obj->length = newsize;
  object_set(thisptr, "length", alloc_int(state, newsize));
  state->result_value = thisptr;
}

static void array_push_fn(VMState *state, Object *thisptr, Object *fn, Object **args_ptr, int args_len) {
  VM_ASSERT(args_len == 1, "wrong arity: expected 1, got %i", args_len);
  Object *array_base = state->shared->vcache.array_base;
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
  Object *array_base = state->shared->vcache.array_base;
  ArrayObject *arr_obj = (ArrayObject*) obj_instance_of(thisptr, array_base);
  VM_ASSERT(arr_obj, "internal error: pop called on object that is not an array");
  Object *res = arr_obj->ptr[arr_obj->length - 1];
  arr_obj->ptr = realloc(arr_obj->ptr, sizeof(Object*) * --arr_obj->length);
  object_set(thisptr, "length", alloc_int(state, arr_obj->length));
  state->result_value = res;
}

static void array_index_fn(VMState *state, Object *thisptr, Object *fn, Object **args_ptr, int args_len) {
  VM_ASSERT(args_len == 1, "wrong arity: expected 1, got %i", args_len);
  Object *int_base = state->shared->vcache.int_base;
  Object *array_base = state->shared->vcache.array_base;
  ArrayObject *arr_obj = (ArrayObject*) obj_instance_of(thisptr, array_base);
  Object *arg = args_ptr[0];
  if (arg->parent != int_base) { state->result_value = NULL; return; }
  VM_ASSERT(arr_obj, "internal error: array '[]' called on object that is not an array");
  int index = arg->int_value;
  VM_ASSERT(index >= 0 && index < arr_obj->length, "array index out of bounds!");
  state->result_value = arr_obj->ptr[index];
}

static void array_index_assign_fn(VMState *state, Object *thisptr, Object *fn, Object **args_ptr, int args_len) {
  VM_ASSERT(args_len == 2, "wrong arity: expected 2, got %i", args_len);
  Object *int_base = state->shared->vcache.int_base;
  Object *array_base = state->shared->vcache.array_base;
  ArrayObject *arr_obj = (ArrayObject*) obj_instance_of(thisptr, array_base);
  Object *arg = args_ptr[0];
  VM_ASSERT(arr_obj, "internal error: array '[]=' called on object that is not an array");
  VM_ASSERT(arg->parent == int_base, "index of array '[]=' must be int");
  int index = arg->int_value;
  VM_ASSERT(index >= 0 && index < arr_obj->length, "array index out of bounds!");
  Object *value = args_ptr[1];
  arr_obj->ptr[index] = value;
  state->result_value = NULL;
}

static void array_join_fn(VMState *state, Object *thisptr, Object *fn, Object **args_ptr, int args_len) {
  VM_ASSERT(args_len == 1, "wrong arity: expected 1, got %i", args_len);
  Object *array_base = state->shared->vcache.array_base;
  Object *string_base = state->shared->vcache.string_base;
  ArrayObject *arr_obj = (ArrayObject*) obj_instance_of(thisptr, array_base);
  StringObject *str_arg = (StringObject*) obj_instance_of(args_ptr[0], string_base);
  VM_ASSERT(arr_obj, "internal error: 'join' called on object that is not an array");
  VM_ASSERT(str_arg, "argument to array.join() must be string");
  
  int joiner_len = strlen(str_arg->value);
  int res_len = 0;
  int *lens = alloca(sizeof(int) * arr_obj->length);
  for (int i = 0; i < arr_obj->length; i++) {
    if (i > 0) res_len += joiner_len;
    Object *entry = arr_obj->ptr[i];
    StringObject *entry_str = (StringObject*) obj_instance_of(entry, string_base);
    VM_ASSERT(entry_str, "contents of array must be strings");
    int len = strlen(entry_str->value);
    res_len += len;
    lens[i] = len;
  }
  char *res = malloc(res_len + 1);
  char *res_cur = res;
  for (int i = 0; i < arr_obj->length; i++) {
    if (i > 0) {
      memcpy(res_cur, str_arg->value, joiner_len);
      res_cur += joiner_len;
    }
    Object *entry = arr_obj->ptr[i];
    StringObject *entry_str = (StringObject*) obj_instance_of(entry, string_base);
    // this is safe - we counted up the length above
    // (assuming nobody changes entry_str under us)
    memcpy(res_cur, entry_str->value, lens[i]);
    res_cur += lens[i];
  }
  res_cur[0] = 0;
  // TODO make string constructor that takes ownership of the pointer instead
  state->result_value = alloc_string(state, res, res_len);
  free(res);
}

static void file_print_fn(VMState *state, Object *thisptr, Object *fn, Object **args_ptr, int args_len) {
  Object *pointer_base = state->shared->vcache.pointer_base;
  Object *file_base = OBJECT_LOOKUP_STRING(state->root, "file", NULL);
  assert(file_base);
  
  VM_ASSERT(obj_instance_of(thisptr, file_base), "print() called on object that is not a file");
  Object *hdl_obj = OBJECT_LOOKUP_STRING(thisptr, "_handle", NULL);
  VM_ASSERT(hdl_obj, "missing _handle!");
  VM_ASSERT(hdl_obj->parent == pointer_base, "_handle must be a pointer!");
  PointerObject *hdl_ptrobj = (PointerObject*) hdl_obj;
  FILE *file = hdl_ptrobj->ptr;
  for (int i = 0; i < args_len; ++i) {
    Object *arg = args_ptr[i];
    print_recursive(state, file, arg, true);
    if (state->runstate == VM_ERRORED) return;
  }
  fprintf(file, "\n");
  state->result_value = NULL;
}

static void file_open_fn(VMState *state, Object *thisptr, Object *fn, Object **args_ptr, int args_len) {
  VM_ASSERT(args_len == 2, "wrong arity: expected 2, got %i", args_len);
  Object *file_base = OBJECT_LOOKUP_STRING(state->root, "file", NULL);
  Object *string_base = state->shared->vcache.string_base;
  assert(file_base);
  
  VM_ASSERT(thisptr == file_base, "open() called on object other than file!");
  StringObject *fnobj = (StringObject*) obj_instance_of(args_ptr[0], string_base);
  VM_ASSERT(fnobj, "first parameter to file.open() must be string!");
  StringObject *fmobj = (StringObject*) obj_instance_of(args_ptr[1], string_base);
  VM_ASSERT(fmobj, "second parameter to file.open() must be string!");
  
  gc_disable(state);
  FILE *fh = fopen(fnobj->value, fmobj->value);
  if (fh == NULL) {
    VM_ASSERT(false, "file could not be opened: '%s' as '%s': %s", fnobj->value, fmobj->value, strerror(errno));
  }
  Object *file_obj = alloc_object(state, file_base);
  object_set(file_obj, "_handle", alloc_ptr(state, (void*) fh));
  state->result_value = file_obj;
  gc_enable(state);
}

static void file_close_fn(VMState *state, Object *thisptr, Object *fn, Object **args_ptr, int args_len) {
  VM_ASSERT(args_len == 0, "wrong arity: expected 0, got %i", args_len);
  Object *file_base = OBJECT_LOOKUP_STRING(state->root, "file", NULL);
  Object *pointer_base = state->shared->vcache.pointer_base;
  assert(file_base);
  
  VM_ASSERT(obj_instance_of(thisptr, file_base), "close() called on object that is not a file!");
  Object *hdl_obj = OBJECT_LOOKUP_STRING(thisptr, "_handle", NULL);
  VM_ASSERT(hdl_obj, "missing _handle!");
  VM_ASSERT(hdl_obj->parent == pointer_base, "_handle must be a pointer!");
  PointerObject *hdl_ptrobj = (PointerObject*) hdl_obj;
  FILE *file = hdl_ptrobj->ptr;
  fclose(file);
  object_set(thisptr, "_handle", NULL);
  
  state->result_value = NULL;
}

static void print_fn(VMState *state, Object *thisptr, Object *fn, Object **args_ptr, int args_len) {
  for (int i = 0; i < args_len; ++i) {
    Object *arg = args_ptr[i];
    print_recursive(state, stdout, arg, true);
    if (state->runstate == VM_ERRORED) return;
  }
  fprintf(stdout, "\n");
  state->result_value = NULL;
}

static void keys_fn(VMState *state, Object *thisptr, Object *fn, Object **args_ptr, int args_len) {
  VM_ASSERT(args_len == 1, "wrong arity: expected 1, got %i", args_len);
  gc_disable(state);
  int res_len = 0;
  Object *obj = args_ptr[0];
  if (!(obj->flags & OBJ_PRIMITIVE)) {
    HashTable *tbl = &obj->tbl;
    for (int i = 0; i < tbl->entries_num; ++i) {
      TableEntry *entry = &tbl->entries_ptr[i];
      if (entry->name_ptr) res_len ++;
    }
  }
  Object **res_ptr = malloc(sizeof(Object*) * res_len);
  if (!(obj->flags & OBJ_PRIMITIVE)) {
    int k = 0;
    HashTable *tbl = &obj->tbl;
    for (int i = 0; i < tbl->entries_num; ++i) {
      TableEntry *entry = &tbl->entries_ptr[i];
      if (entry->name_ptr) {
        res_ptr[k++] = alloc_string(state, entry->name_ptr, strlen(entry->name_ptr));
      }
    }
  }
  state->result_value = alloc_array(state, res_ptr, alloc_int(state, res_len));
  gc_enable(state);
}

static Object *xml_to_object(VMState *state, xmlNode *element, Object *text_node_base, Object *element_node_base) {
  Object *res;
  if (element->type == 1) {
    res = alloc_object(state, element_node_base);
    xmlNode *child = element->children;
    int children_len = 0;
    for (; child; child = child->next) children_len ++;
    Object **children_ptr = malloc(sizeof(Object*) * children_len);
    int i = 0;
    for (child = element->children; child; child = child->next) {
      children_ptr[i++] = xml_to_object(state, child, text_node_base, element_node_base);
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
      char *content = (char*) xml_attr->children->content;
      object_set(attr, name2, alloc_string(state, content, strlen(content)));
    }
    // printf("alloc_string(%lu)\n", strlen((char*) element->name));
    Object *children_len_obj = alloc_int(state, children_len);
    object_set(res, "nodeName", alloc_string(state, (char*) element->name, strlen((char*) element->name)));
    object_set(res, "attr", attr);
    object_set(res, "children", alloc_array(state, children_ptr, children_len_obj));
  } else if (element->type == 3) {
    res = alloc_object(state, text_node_base);
    // printf("alloc_string(%lu)\n", strlen((char*) element->content));
    char *content = (char*) element->content;
    object_set(res, "value", alloc_string(state, content, strlen(content)));
  } else abort();
  return res;
}

static void xml_load_fn(VMState *state, Object *thisptr, Object *fn, Object **args_ptr, int args_len) {
  VM_ASSERT(args_len == 1, "wrong arity: expected 1, got %i", args_len);
  Object *root = state->root;
  Object *string_base = state->shared->vcache.string_base;
  Object *xml_base = OBJECT_LOOKUP_STRING(root, "xml", NULL);
  Object *text_node_base = OBJECT_LOOKUP_STRING(xml_base, "text_node", NULL);
  Object *element_node_base = OBJECT_LOOKUP_STRING(xml_base, "element_node", NULL);
  
  StringObject *str_obj = (StringObject*) obj_instance_of(args_ptr[0], string_base);
  VM_ASSERT(str_obj, "parameter to xml.load must be string");
  char *file = str_obj->value;
  
  LIBXML_TEST_VERSION
  
  xmlDoc *doc = xmlReadFile(file, NULL, 0);
  VM_ASSERT(doc != NULL, "cannot read xml file");
  
  xmlNode *root_element = xmlDocGetRootElement(doc);
  
  gc_disable(state);
  
  state->result_value = xml_to_object(state, root_element, text_node_base, element_node_base);
  gc_enable(state);
  
  xmlFreeDoc(doc);
  xmlCleanupParser();
}

static void xml_parse_fn(VMState *state, Object *thisptr, Object *fn, Object **args_ptr, int args_len) {
  VM_ASSERT(args_len == 1, "wrong arity: expected 1, got %i", args_len);
  Object *root = state->root;
  Object *string_base = state->shared->vcache.string_base;
  Object *xml_base = OBJECT_LOOKUP_STRING(root, "xml", NULL);
  Object *text_node_base = OBJECT_LOOKUP_STRING(xml_base, "text_node", NULL);
  Object *element_node_base = OBJECT_LOOKUP_STRING(xml_base, "element_node", NULL);
  
  StringObject *str_obj = (StringObject*) obj_instance_of(args_ptr[0], string_base);
  VM_ASSERT(str_obj, "parameter to xml.parse must be string");
  char *text = str_obj->value;
  
  LIBXML_TEST_VERSION
  
  xmlDoc *doc = xmlReadMemory(text, strlen(text), NULL, NULL, 0);
  VM_ASSERT(doc != NULL, "failed to parse XML string");
  
  xmlNode *root_element = xmlDocGetRootElement(doc);
  
  gc_disable(state);
  
  state->result_value = xml_to_object(state, root_element, text_node_base, element_node_base);
  gc_enable(state);
  
  xmlFreeDoc(doc);
  xmlCleanupParser();
}

static bool xml_node_check_pred(VMState *state, Object *node, Object *pred)
{
  VMState substate = {0};
  substate.parent = state;
  substate.root = state->root;
  substate.shared = state->shared;
  
  if (!setup_call(&substate, node, pred, &node, 1)) {
    VM_ASSERT(false, "pred check failure: %s\n", substate.error) false;
  }
  
  vm_run(&substate);
  VM_ASSERT(substate.runstate != VM_ERRORED, "pred check failure: %s\n", substate.error) false;
  
  Object *res = substate.result_value;
  return obj_is_truthy(state, res);
}

static void xml_node_find_recurse(VMState *state, Object *node, Object *pred, Object ***array_p_p, int *array_l_p)
{
  Object *array_base = state->shared->vcache.array_base;
  bool res = xml_node_check_pred(state, node, pred);
  if (state->runstate == VM_ERRORED) return;
  if (res) {
    (*array_p_p) = realloc((void*) *array_p_p, sizeof(Object*) * ++(*array_l_p));
    (*array_p_p)[(*array_l_p) - 1] = node;
  }
  
  Object *children_obj = OBJECT_LOOKUP_STRING(node, "children", NULL);
  VM_ASSERT(children_obj, "missing 'children' property in node");
  ArrayObject *children_aobj = (ArrayObject*) obj_instance_of(children_obj, array_base);
  VM_ASSERT(children_aobj, "'children' property in node is not an array");
  for (int i = 0; i < children_aobj->length; ++i) {
    Object *child = children_aobj->ptr[i];
    xml_node_find_recurse(state, child, pred, array_p_p, array_l_p);
    if (state->runstate == VM_ERRORED) return;
  }
}

static void xml_node_find_array_fn(VMState *state, Object *thisptr, Object *fn, Object **args_ptr, int args_len) {
  VM_ASSERT(args_len == 1, "wrong arity: expected 1, got %i", args_len);
  
  Object **array_ptr = NULL; int array_length = 0;
  gc_disable(state);
  xml_node_find_recurse(state, thisptr, args_ptr[0], &array_ptr, &array_length);
  Object *array_len_obj = alloc_int(state, array_length);
  state->result_value = alloc_array(state, array_ptr, array_len_obj);
  gc_enable(state);
}

static void xml_node_find_by_name_recurse(VMState *state, Object *node, char *name, Object ***array_p_p, int *array_l_p)
{
  Object *int_base = state->shared->vcache.int_base;
  Object *string_base = state->shared->vcache.string_base;
  Object *array_base = state->shared->vcache.array_base;
  Object *node_type = OBJECT_LOOKUP_STRING(node, "nodeType", NULL);
  VM_ASSERT(node_type && node_type->parent == int_base, "invalid xml node");
  if (node_type->int_value == 3) return; // text
  VM_ASSERT(node_type->int_value == 1, "node is not element");
  
  Object *node_name = OBJECT_LOOKUP_STRING(node, "nodeName", NULL);
  
  VM_ASSERT(node_name, "missing 'nodeName' property in node");
  StringObject *nodeName_str = (StringObject*) obj_instance_of(node_name, string_base);
  if (strcmp(nodeName_str->value, name) == 0) {
    (*array_p_p) = realloc((void*) *array_p_p, sizeof(Object*) * ++(*array_l_p));
    (*array_p_p)[(*array_l_p) - 1] = node;
  }
  
  Object *children_obj = OBJECT_LOOKUP_STRING(node, "children", NULL);
  VM_ASSERT(children_obj, "missing 'children' property in node");
  ArrayObject *children_aobj = (ArrayObject*) obj_instance_of(children_obj, array_base);
  VM_ASSERT(children_aobj, "'children' property in node is not an array");
  for (int i = 0; i < children_aobj->length; ++i) {
    Object *child = children_aobj->ptr[i];
    xml_node_find_by_name_recurse(state, child, name, array_p_p, array_l_p);
    if (state->runstate == VM_ERRORED) return;
  }
}

static void xml_node_find_by_name_array_fn(VMState *state, Object *thisptr, Object *fn, Object **args_ptr, int args_len) {
  VM_ASSERT(args_len == 1, "wrong arity: expected 1, got %i", args_len);
  Object *string_base = state->shared->vcache.string_base;
  
  StringObject *name_obj = (StringObject*) obj_instance_of(args_ptr[0], string_base);
  VM_ASSERT(name_obj, "parameter to find_array_by_name must be string!");
  
  Object **array_ptr = NULL; int array_length = 0;
  gc_disable(state);
  xml_node_find_by_name_recurse(state, thisptr, name_obj->value, &array_ptr, &array_length);
  Object *array_len_obj = alloc_int(state, array_length);
  state->result_value = alloc_array(state, array_ptr, array_len_obj);
  gc_enable(state);
}

#include "language.h"

static void require_fn(VMState *state, Object *thisptr, Object *fn, Object **args_ptr, int args_len) {
  VM_ASSERT(args_len == 1, "wrong arity: expected 1, got %i", args_len);
  Object *root = state->root;
  Object *string_base = state->shared->vcache.string_base;
  
  StringObject *file_obj = (StringObject*) obj_instance_of(args_ptr[0], string_base);
  VM_ASSERT(file_obj, "parameter to require() must be string!");
  
  char *filename = file_obj->value;
  
  TextRange source = readfile(filename);
  register_file(source, my_asprintf("%s", filename) /* dup */, 0, 0);
  
  UserFunction *module;
  char *text = source.start;
  ParseResult res = parse_module(&text, &module);
  VM_ASSERT(res == PARSE_OK, "require() parsing failed!");
  // dump_fn(module);
  
  VMState substate = {0};
  substate.parent = state;
  substate.root = state->root;
  substate.shared = state->shared;
  
  call_function(&substate, root, module, NULL, 0);
  vm_run(&substate);
  
  if (substate.runstate == VM_ERRORED) {
    state->runstate = VM_ERRORED;
    state->error = my_asprintf("Error during require('%s')\n%s", filename, substate.error);
    state->backtrace = vm_record_backtrace(&substate, &state->backtrace_depth);
    free(substate.backtrace);
    return;
  }
  
  state->result_value = substate.result_value;
}

static void freeze_fn(VMState *state, Object *thisptr, Object *fn, Object **args_ptr, int args_len) {
  VM_ASSERT(args_len == 1, "wrong arity: expected 1, got %i", args_len);
  Object *obj = args_ptr[0];
  obj->flags |= OBJ_FROZEN;
}

static void mark_const_fn(VMState *state, Object *thisptr, Object *fn, Object **args_ptr, int args_len) {
  VM_ASSERT(args_len == 1, "wrong arity: expected 1, got %i", args_len);
  Object *string_base = state->shared->vcache.string_base;
  StringObject *key_obj = (StringObject*) obj_instance_of(args_ptr[0], string_base);
  VM_ASSERT(key_obj, "argument to _mark_const must be string");
  
  char *key_ptr = key_obj->value;
  int key_len = strlen(key_ptr);
  size_t key_hash = hash(key_ptr, key_len);
  
  state->result_value = NULL;
  
  // frames are only allocated for user functions
  // so we're still in the calling frame
  Callframe *cf = state->frame;
  int context_slot = cf->instr_ptr->context_slot;
  Object *context = cf->slots_ptr[context_slot];
  
  Object *cur = context;
  if (cur->flags & OBJ_PRIMITIVE) cur = cur->parent; // known to be non-primitive from here up because noinherit
  while (cur) {
    TableEntry *entry = table_lookup_with_hash(&cur->tbl, key_ptr, key_len, key_hash);
    if (entry) {
      VM_ASSERT(cur->tbl.entries_stored == 1, "more than one var in this scope: something bad has happened??");
      cur->flags |= OBJ_FROZEN;
      return;
    }
    cur = cur->parent;
  }
  VM_ASSERT(false, "cannot mark const: variable not found");
}

static void obj_keys_fn(VMState *state, Object *thisptr, Object *fn, Object **args_ptr, int args_len) {
  VM_ASSERT(args_len == 1, "wrong arity: expected 1, got %i", args_len);
  Object *obj = args_ptr[0];
  int keys_len = 0;
  Object **keys_ptr = NULL;
  if (!(obj->flags & OBJ_PRIMITIVE)) {
    keys_len = obj->tbl.entries_stored;
    keys_ptr = malloc(sizeof(Object*) * keys_len);
    int k = 0;
    for (int i = 0; i < obj->tbl.entries_num; i++) {
      const char *name_ptr = obj->tbl.entries_ptr[i].name_ptr;
      int name_len = obj->tbl.entries_ptr[i].name_len;
      if (name_ptr) {
        keys_ptr[k++] = alloc_string(state, name_ptr, name_len);
      }
    }
    assert(k == keys_len);
  }
  state->result_value = alloc_array(state, keys_ptr, alloc_int(state, keys_len));
}

static void obj_instanceof_fn(VMState *state, Object *thisptr, Object *fn, Object **args_ptr, int args_len) {
  VM_ASSERT(args_len == 2, "wrong arity: expected 2, got %i", args_len);
  Object *obj = args_ptr[0];
  Object *parent_obj = args_ptr[1];
  VM_ASSERT(parent_obj, "bad argument: instanceof null");
  bool res = !!obj_instance_of(obj, parent_obj);
  state->result_value = alloc_bool(state, res);
}

static void sin_fn(VMState *state, Object *thisptr, Object *fn, Object **args_ptr, int args_len) {
  VM_ASSERT(args_len == 1, "wrong arity: expected 1, got %i", args_len);
  float f;
  if (args_ptr[0]->parent == state->shared->vcache.float_base) f = args_ptr[0]->float_value;
  else if (args_ptr[0]->parent == state->shared->vcache.int_base) f = args_ptr[0]->int_value;
  else VM_ASSERT(false, "unexpected type for math.sin()");
  state->result_value = alloc_float(state, sinf(f));
}

static void cos_fn(VMState *state, Object *thisptr, Object *fn, Object **args_ptr, int args_len) {
  VM_ASSERT(args_len == 1, "wrong arity: expected 1, got %i", args_len);
  float f;
  if (args_ptr[0]->parent == state->shared->vcache.float_base) f = args_ptr[0]->float_value;
  else if (args_ptr[0]->parent == state->shared->vcache.int_base) f = args_ptr[0]->int_value;
  else VM_ASSERT(false, "unexpected type for math.cos()");
  state->result_value = alloc_float(state, cosf(f));
}

static void tan_fn(VMState *state, Object *thisptr, Object *fn, Object **args_ptr, int args_len) {
  VM_ASSERT(args_len == 1, "wrong arity: expected 1, got %i", args_len);
  float f;
  if (args_ptr[0]->parent == state->shared->vcache.float_base) f = args_ptr[0]->float_value;
  else if (args_ptr[0]->parent == state->shared->vcache.int_base) f = args_ptr[0]->int_value;
  else VM_ASSERT(false, "unexpected type for math.tan()");
  state->result_value = alloc_float(state, tanf(f));
}

static void sqrt_fn(VMState *state, Object *thisptr, Object *fn, Object **args_ptr, int args_len) {
  VM_ASSERT(args_len == 1, "wrong arity: expected 1, got %i", args_len);
  float f;
  if (args_ptr[0]->parent == state->shared->vcache.float_base) f = args_ptr[0]->float_value;
  else if (args_ptr[0]->parent == state->shared->vcache.int_base) f = args_ptr[0]->int_value;
  else VM_ASSERT(false, "unexpected type for math.sqrt()");
  state->result_value = alloc_float(state, sqrtf(f));
}

static void pow_fn(VMState *state, Object *thisptr, Object *fn, Object **args_ptr, int args_len) {
  VM_ASSERT(args_len == 2, "wrong arity: expected 2, got %i", args_len);
  float a, b;
  if (args_ptr[0]->parent == state->shared->vcache.float_base) a = args_ptr[0]->float_value;
  else if (args_ptr[0]->parent == state->shared->vcache.int_base) a = args_ptr[0]->int_value;
  VM_ASSERT(false, "unexpected type for math.pow()");
  if (args_ptr[1]->parent == state->shared->vcache.float_base) b = args_ptr[1]->float_value;
  else if (args_ptr[1]->parent == state->shared->vcache.int_base) b = args_ptr[1]->int_value;
  VM_ASSERT(false, "unexpected type for math.pow()");
  state->result_value = alloc_float(state, powf(a, b));
}

static void assert_fn(VMState *state, Object *thisptr, Object *fn, Object **args_ptr, int args_len) {
  VM_ASSERT(args_len == 1 || args_len == 2, "wrong arity: expected 1 or 2, got %i", args_len);
  bool test = obj_is_truthy(state, args_ptr[0]);
  if (args_len == 2) {
    Object *string_base = state->shared->vcache.string_base;
    StringObject *msg_obj = (StringObject*) obj_instance_of(args_ptr[1], string_base);
    VM_ASSERT(msg_obj, "second parameter to assert() must be string");
    VM_ASSERT(test, "assert failed: %s", msg_obj->value);
  } else {
    VM_ASSERT(test, "assert failed");
  }
  state->result_value = NULL;
}

char *get_type_info(VMState *state, Object *obj) {
  if (obj == NULL) return "null";
  if (!state) return my_asprintf("%p", (void*) obj);
  if (obj == state->shared->vcache.int_base) return "int";
  if (obj == state->shared->vcache.bool_base) return "bool";
  if (obj == state->shared->vcache.float_base) return "float";
  if (obj == state->shared->vcache.closure_base) return "closure";
  if (obj == state->shared->vcache.function_base) return "function";
  if (obj == state->shared->vcache.array_base) return "array";
  if (obj == state->shared->vcache.string_base) return "string";
  if (obj == state->shared->vcache.pointer_base) return "pointer";
  
  if (obj->parent) return get_type_info(state, obj->parent);
  return "unknown";
}

Object *create_root(VMState *state) {
  Object *root = alloc_object(state, NULL);
  
  state->root = root;
  
  GCRootSet pin_root;
  gc_add_roots(state, &root, 1, &pin_root);
  
  Object *function_obj = alloc_object(state, NULL);
  function_obj->flags |= OBJ_NOINHERIT;
  object_set(root, "function", function_obj);
  state->shared->vcache.function_base = function_obj;
  object_set(function_obj, "apply", alloc_fn(state, fn_apply_fn));
  
  Object *int_obj = alloc_object(state, NULL);
  int_obj->flags |= OBJ_NOINHERIT;
  object_set(root, "int", int_obj);
  object_set(int_obj, "+" , alloc_fn(state, int_add_fn));
  object_set(int_obj, "-" , alloc_fn(state, int_sub_fn));
  object_set(int_obj, "*" , alloc_fn(state, int_mul_fn));
  object_set(int_obj, "/" , alloc_fn(state, int_div_fn));
  object_set(int_obj, "|" , alloc_fn(state, int_bit_or_fn));
  object_set(int_obj, "&" , alloc_fn(state, int_bit_and_fn));
  object_set(int_obj, "==", alloc_fn(state, int_eq_fn));
  object_set(int_obj, "<" , alloc_fn(state, int_lt_fn));
  object_set(int_obj, ">" , alloc_fn(state, int_gt_fn));
  object_set(int_obj, "<=", alloc_fn(state, int_le_fn));
  object_set(int_obj, ">=", alloc_fn(state, int_ge_fn));
  object_set(int_obj, "parse" , alloc_fn(state, int_parse_fn));
  state->shared->vcache.int_base = int_obj;
  int_obj->flags |= OBJ_FROZEN;
  state->shared->vcache.int_zero = alloc_int(state, 0);
  gc_add_perm(state, state->shared->vcache.int_zero);
  
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
  object_set(float_obj, "toInt" , alloc_fn(state, float_toint_fn));
  state->shared->vcache.float_base = float_obj;
  float_obj->flags |= OBJ_FROZEN;
  
  Object *closure_obj = alloc_object(state, NULL);
  closure_obj->flags |= OBJ_NOINHERIT;
  object_set(root, "closure", closure_obj);
  object_set(closure_obj, "apply", alloc_fn(state, fn_apply_fn));
  state->shared->vcache.closure_base = closure_obj;
  
  Object *bool_obj = alloc_object(state, NULL);
  bool_obj->flags |= OBJ_NOINHERIT;
  object_set(root, "bool", bool_obj);
  object_set(bool_obj, "!", alloc_fn(state, bool_not_fn));
  object_set(bool_obj, "==", alloc_fn(state, bool_eq_fn));
  state->shared->vcache.bool_base = bool_obj;
  bool_obj->flags |= OBJ_FROZEN;
  
  Object
    *true_obj = alloc_bool_uncached(state, true),
    *false_obj = alloc_bool_uncached(state, false);
  object_set(root, "true", true_obj);
  object_set(root, "false", false_obj);
  state->shared->vcache.bool_false = false_obj;
  state->shared->vcache.bool_true = true_obj;
  
  object_set(root, "null", NULL);
  
  Object *string_obj = alloc_object(state, NULL);
  string_obj->flags |= OBJ_NOINHERIT;
  object_set(root, "string", string_obj);
  object_set(string_obj, "+", alloc_fn(state, string_add_fn));
  object_set(string_obj, "==", alloc_fn(state, string_eq_fn));
  object_set(string_obj, "startsWith", alloc_fn(state, string_startswith_fn));
  object_set(string_obj, "endsWith", alloc_fn(state, string_endswith_fn));
  object_set(string_obj, "slice", alloc_fn(state, string_slice_fn));
  object_set(string_obj, "find", alloc_fn(state, string_find_fn));
  object_set(string_obj, "byte_len", alloc_fn(state, string_byte_len_fn));
  state->shared->vcache.string_base = string_obj;
  
  Object *array_obj = alloc_object(state, NULL);
  array_obj->flags |= OBJ_NOINHERIT;
  object_set(root, "array", array_obj);
  object_set(array_obj, "resize", alloc_fn(state, array_resize_fn));
  object_set(array_obj, "push", alloc_fn(state, array_push_fn));
  object_set(array_obj, "pop", alloc_fn(state, array_pop_fn));
  object_set(array_obj, "[]", alloc_fn(state, array_index_fn));
  object_set(array_obj, "[]=", alloc_fn(state, array_index_assign_fn));
  object_set(array_obj, "join", alloc_fn(state, array_join_fn));
  state->shared->vcache.array_base = array_obj;
  
  Object *ptr_obj = alloc_object(state, NULL);
  ptr_obj->flags |= OBJ_NOINHERIT;
  object_set(root, "pointer", ptr_obj);
  object_set(ptr_obj, "null", alloc_fn(state, ptr_is_null_fn));
  state->shared->vcache.pointer_base = ptr_obj;
  
  object_set(root, "keys", alloc_fn(state, keys_fn));
  
  Object *xml_obj = alloc_object(state, NULL);
  object_set(xml_obj, "load", alloc_fn(state, xml_load_fn));
  object_set(xml_obj, "parse", alloc_fn(state, xml_parse_fn));
  
  Object *node_obj = alloc_object(state, NULL);
  object_set(node_obj, "find_array", alloc_fn(state, xml_node_find_array_fn));
  object_set(node_obj, "find_array_by_name", alloc_fn(state, xml_node_find_by_name_array_fn));
  xml_obj->flags |= OBJ_FROZEN;
  object_set(xml_obj, "node", node_obj);
  
  Object *element_node_obj = alloc_object(state, node_obj);
  object_set(element_node_obj, "nodeName", alloc_string_foreign(state, ""));
  object_set(element_node_obj, "nodeType", alloc_int(state, 1));
  object_set(element_node_obj, "attr", alloc_object(state, NULL));
  object_set(element_node_obj, "children", alloc_array(state, NULL, state->shared->vcache.int_zero));
  object_set(xml_obj, "element_node", element_node_obj);
  
  Object *text_node_obj = alloc_object(state, node_obj);
  object_set(text_node_obj, "nodeType", alloc_int(state, 3));
  object_set(text_node_obj, "value", NULL);
  object_set(xml_obj, "text_node", text_node_obj);
  
  object_set(root, "xml", xml_obj);
  
  Object *file_obj = alloc_object(state, NULL);
  object_set(file_obj, "_handle", NULL);
  object_set(file_obj, "print", alloc_fn(state, file_print_fn));
  object_set(file_obj, "open", alloc_fn(state, file_open_fn));
  object_set(file_obj, "close", alloc_fn(state, file_close_fn));
  file_obj->flags |= OBJ_FROZEN;
  object_set(root, "file", file_obj);
  
  Object *stdout_obj = alloc_object(state, file_obj);
  object_set(stdout_obj, "_handle", alloc_ptr(state, (void*) stdout));
  object_set(root, "stdout", stdout_obj);
  
  Object *stderr_obj = alloc_object(state, file_obj);
  object_set(stderr_obj, "_handle", alloc_ptr(state, (void*) stderr));
  object_set(root, "stderr", stderr_obj);
  
  object_set(root, "print", alloc_fn(state, print_fn));
  
  object_set(root, "require", alloc_fn(state, require_fn));
  object_set(root, "freeze", alloc_fn(state, freeze_fn));
  object_set(root, "_mark_const", alloc_fn(state, mark_const_fn));
  object_set(root, "assert", alloc_fn(state, assert_fn));
  
  Object *math_obj = alloc_object(state, NULL);
  object_set(math_obj, "sin", alloc_fn(state, sin_fn));
  object_set(math_obj, "cos", alloc_fn(state, cos_fn));
  object_set(math_obj, "tan", alloc_fn(state, tan_fn));
  object_set(math_obj, "sqrt", alloc_fn(state, sqrt_fn));
  object_set(math_obj, "pow", alloc_fn(state, pow_fn));
  math_obj->flags |= OBJ_FROZEN;
  object_set(root, "math", math_obj);
  
  Object *obj_tools = alloc_object(state, NULL);
  obj_tools->flags |= OBJ_NOINHERIT;
  object_set(obj_tools, "keys", alloc_fn(state, obj_keys_fn));
  object_set(obj_tools, "instanceof", alloc_fn(state, obj_instanceof_fn));
  
  object_set(root, "object", obj_tools);
  
  ffi_setup_root(state, root);
  
  root->flags |= OBJ_FROZEN;
  gc_remove_roots(state, &pin_root);
  
  return root;
}
