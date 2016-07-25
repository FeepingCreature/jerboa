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

static void fn_apply_fn(VMState *state, CallInfo *info) {
  VM_ASSERT(info->args_len == 1, "wrong arity: expected 1, got %i", info->args_len);
  Object *array_base = state->shared->vcache.array_base;
  ArrayObject *args_array = (ArrayObject*) obj_instance_of(OBJ_OR_NULL(load_arg(state->frame, INFO_ARGS_PTR(info)[0])), array_base);
  VM_ASSERT(args_array, "argument to apply() must be array!");
  int len = args_array->length;
  Value fn_value = load_arg(state->frame, info->this_arg);
  
  setup_stub_frame(state, len);
  state->frame->target = info->target;
  for (int i = 0; i < len; ++i) {
    state->frame->slots_ptr[i] = args_array->ptr[i];
  }
  
  CallInfo *info2 = alloca(sizeof(CallInfo) + sizeof(Arg) * len);
  info2->args_len = len;
  info2->this_arg = (Arg) { .kind = ARG_VALUE, .value = VNULL };
  info2->fn = (Arg) { .kind = ARG_VALUE, .value = fn_value };
  info2->target = (WriteArg) { .kind = ARG_SLOT, .slot = len }; // len is the return slot
  for (int i = 0; i < len; ++i) {
    INFO_ARGS_PTR(info2)[i] = (Arg) { .kind = ARG_SLOT, .slot = i };
  }
  // passthrough call to actual function
  // note: may set its own errors
  setup_call(state, info2);
}

// TODO add "IS_TRUTHY" instr so we can promote, say, null, to bool before calling this
static void bool_not_fn(VMState *state, CallInfo *info) {
  VM_ASSERT(info->args_len == 0, "wrong arity: expected 0, got %i", info->args_len);
  Value this_val = load_arg(state->frame, info->this_arg);
  VM_ASSERT(IS_BOOL(this_val), "internal error: bool negation called on wrong type of object");
  vm_return(state, info, make_bool(state, !AS_BOOL(this_val)));
}

static void bool_eq_fn(VMState *state, CallInfo *info) {
  VM_ASSERT(info->args_len == 1, "wrong arity: expected 1, got %i", info->args_len);
  
  Value val1 = load_arg(state->frame, info->this_arg), val2 = load_arg(state->frame, INFO_ARGS_PTR(info)[0]);
  
  VM_ASSERT(IS_BOOL(val1), "internal error: bool compare function called on wrong type of object");
  VM_ASSERT(IS_BOOL(val2), "can't compare bool with this value");
  vm_return(state, info, make_bool(state, AS_BOOL(val1) == AS_BOOL(val2)));
}

typedef enum {
  MATH_ADD,
  MATH_SUB,
  MATH_MUL,
  MATH_DIV,
  MATH_BIT_OR,
  MATH_BIT_AND
} MathOp;

static void int_math_fn(VMState *state, CallInfo *info, MathOp mop) {
  VM_ASSERT(info->args_len == 1, "wrong arity: expected 1, got %i", info->args_len);
  
  Value this_val = load_arg(state->frame, info->this_arg);
  VM_ASSERT(IS_INT(this_val), "internal error: int math function called on wrong type of object"); // otherwise how are we called on it??
  Value val1 = load_arg(state->frame, info->this_arg);
  Value val2 = load_arg(state->frame, INFO_ARGS_PTR(info)[0]);
  
  if (LIKELY(IS_INT(val2))) {
    int i1 = AS_INT(val1), i2 = AS_INT(val2);
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
    vm_return(state, info, INT2VAL(res));
    return;
  }
  
  if (LIKELY(IS_FLOAT(val2))) {
    float v1 = AS_INT(val1), v2 = AS_FLOAT(val2);
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
    vm_return(state, info, FLOAT2VAL(res));
    return;
  }
  vm_error(state, "don't know how to perform int math with %s", get_type_info(state, val2));
}

static void int_add_fn(VMState *state, CallInfo *info) {
  int_math_fn(state, info, MATH_ADD);
}

static void int_sub_fn(VMState *state, CallInfo *info) {
  int_math_fn(state, info, MATH_SUB);
}

static void int_mul_fn(VMState *state, CallInfo *info) {
  int_math_fn(state, info, MATH_MUL);
}

static void int_div_fn(VMState *state, CallInfo *info) {
  int_math_fn(state, info, MATH_DIV);
}

static void int_bit_or_fn(VMState *state, CallInfo *info) {
  int_math_fn(state, info, MATH_BIT_OR);
}

static void int_bit_and_fn(VMState *state, CallInfo *info) {
  int_math_fn(state, info, MATH_BIT_AND);
}

static void int_parse_fn(VMState *state, CallInfo *info) {
  VM_ASSERT(info->args_len == 1, "wrong arity: expected 1, got %i", info->args_len);
  Object *string_base = state->shared->vcache.string_base;
  
  StringObject *sobj = (StringObject*) obj_instance_of(OBJ_OR_NULL(load_arg(state->frame, INFO_ARGS_PTR(info)[0])), string_base);
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
  vm_return(state, info, INT2VAL(res));
}

static void float_math_fn(VMState *state, CallInfo *info, MathOp mop) {
  VM_ASSERT(info->args_len == 1, "wrong arity: expected 1, got %i", info->args_len);
  
  Value obj1 = load_arg(state->frame, info->this_arg);
  Value obj2 = load_arg(state->frame, INFO_ARGS_PTR(info)[0]);
  VM_ASSERT(IS_FLOAT(obj1), "internal error: float math function called on wrong type of object");
  
  float v1 = AS_FLOAT(obj1), v2;
  if (IS_FLOAT(obj2)) v2 = AS_FLOAT(obj2);
  else if (IS_INT(obj2)) v2 = AS_INT(obj2);
  else { vm_error(state, "don't know how to perform float math with %s", get_type_info(state, obj2)); return; }
  
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
  vm_return(state, info, FLOAT2VAL(res));
  return;
}

static void float_add_fn(VMState *state, CallInfo *info) {
  float_math_fn(state, info, MATH_ADD);
}

static void float_sub_fn(VMState *state, CallInfo *info) {
  float_math_fn(state, info, MATH_SUB);
}

static void float_mul_fn(VMState *state, CallInfo *info) {
  float_math_fn(state, info, MATH_MUL);
}

static void float_div_fn(VMState *state, CallInfo *info) {
  float_math_fn(state, info, MATH_DIV);
}

static void string_add_fn(VMState *state, CallInfo *info) {
  VM_ASSERT(info->args_len == 1, "wrong arity: expected 1, got %i", info->args_len);
  
  Value val1 = load_arg(state->frame, info->this_arg);
  Value val2 = load_arg(state->frame, INFO_ARGS_PTR(info)[0]);
  Object
    *sobj1 = obj_instance_of(OBJ_OR_NULL(val1), state->shared->vcache.string_base),
    *sobj2 = obj_instance_of(OBJ_OR_NULL(val2), state->shared->vcache.string_base);
  VM_ASSERT(sobj1, "internal error: string concat function called on wrong type of object");
  
  char *str1 = ((StringObject*) sobj1)->value, *str2;
  if (sobj2) str2 = my_asprintf("%s", ((StringObject*) sobj2)->value);
  else if (IS_FLOAT(val2)) str2 = my_asprintf("%f", AS_FLOAT(val2));
  else if (IS_BOOL(val2)) if (AS_BOOL(val2)) str2 = my_asprintf("%s", "true"); else str2 = my_asprintf("%s", "false");
  else if (IS_INT(val2)) str2 = my_asprintf("%i", AS_INT(val2));
  else VM_ASSERT(false, "don't know how to format object: %p", INFO_ARGS_PTR(info)[0]);
  char *str3 = my_asprintf("%s%s", str1, str2);
  free(str2);
  vm_return(state, info, make_string(state, str3, strlen(str3)));
  free(str3);
}

static void string_eq_fn(VMState *state, CallInfo *info) {
  VM_ASSERT(info->args_len == 1, "wrong arity: expected 1, got %i", info->args_len);
  Object *string_base = state->shared->vcache.string_base;
  
  Object
    *sobj1 = obj_instance_of(OBJ_OR_NULL(load_arg(state->frame, info->this_arg)), string_base),
    *sobj2 = obj_instance_of(OBJ_OR_NULL(load_arg(state->frame, INFO_ARGS_PTR(info)[0])), string_base);
  VM_ASSERT(sobj1, "internal error: string compare function called on wrong type of object");
  VM_ASSERT(sobj2, "can only compare strings with strings!");
  
  char
    *str1 = ((StringObject*) sobj1)->value,
    *str2 = ((StringObject*) sobj2)->value;
  int res = strcmp(str1, str2);
  vm_return(state, info, BOOL2VAL(res == 0));
}

static void string_startswith_fn(VMState *state, CallInfo *info) {
  VM_ASSERT(info->args_len == 1, "wrong arity: expected 1, got %i", info->args_len);
  Object *string_base = state->shared->vcache.string_base;
  
  Object
    *sobj1 = obj_instance_of(OBJ_OR_NULL(load_arg(state->frame, info->this_arg)), string_base),
    *sobj2 = obj_instance_of(OBJ_OR_NULL(load_arg(state->frame, INFO_ARGS_PTR(info)[0])), string_base);
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
    vm_return(state, info, make_string(state, str1 + len2, strlen(str1) - len2));
  } else {
    vm_return(state, info, VNULL);
  }
}

static void string_endswith_fn(VMState *state, CallInfo *info) {
  VM_ASSERT(info->args_len == 1, "wrong arity: expected 1, got %i", info->args_len);
  Object *string_base = state->shared->vcache.string_base;
  
  Object
    *sobj1 = obj_instance_of(OBJ_OR_NULL(load_arg(state->frame, info->this_arg)), string_base),
    *sobj2 = obj_instance_of(OBJ_OR_NULL(load_arg(state->frame, INFO_ARGS_PTR(info)[0])), string_base);
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
    vm_return(state, info, make_string(state, str1, strlen(str1) - len2));
  } else {
    vm_return(state, info, VNULL);
  }
}

static void string_slice_fn(VMState *state, CallInfo *info) {
  VM_ASSERT(info->args_len == 1 || info->args_len == 2, "wrong arity: expected 1 or 2, got %i", info->args_len);
  Object *string_base = state->shared->vcache.string_base;
  
  StringObject *sobj = (StringObject*) obj_instance_of(OBJ_OR_NULL(load_arg(state->frame, info->this_arg)), string_base);
  VM_ASSERT(sobj, "internal error: string.slice() called on wrong type of object");
  
  char *str = sobj->value;
  int len = utf8_strlen(str);
  int from = 0, to = len;
  if (info->args_len == 1) {
    Value arg1 = load_arg(state->frame, INFO_ARGS_PTR(info)[0]);
    VM_ASSERT(IS_INT(arg1), "string.slice() expected int");
    from = AS_INT(arg1);
  } else {
    Value arg1 = load_arg(state->frame, INFO_ARGS_PTR(info)[0]), arg2 = load_arg(state->frame, INFO_ARGS_PTR(info)[1]);
    VM_ASSERT(IS_INT(arg1), "string.slice() expected int as first parameter");
    VM_ASSERT(IS_INT(arg2), "string.slice() expected int as second parameter");
    from = AS_INT(arg1);
    to = AS_INT(arg2);
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
  
  vm_return(state, info, make_string(state, start, end - start));
}

static void string_find_fn(VMState *state, CallInfo *info) {
  VM_ASSERT(info->args_len == 1, "wrong arity: expected 1, got %i", info->args_len);
  Object *string_base = state->shared->vcache.string_base;
  
  StringObject *sobj = (StringObject*) obj_instance_of(OBJ_OR_NULL(load_arg(state->frame, info->this_arg)), string_base);
  VM_ASSERT(sobj, "internal error: string.find() called on wrong type of object");
  StringObject *sobj2 = (StringObject*) obj_instance_of(OBJ_OR_NULL(load_arg(state->frame, INFO_ARGS_PTR(info)[0])), string_base);
  VM_ASSERT(sobj2, "internal error: string.find() expects string");
  
  char *str = sobj->value;
  int len = strlen(str);
  char *match = sobj2->value;
  int matchlen = strlen(match);
  if (matchlen == 0) {
    vm_return(state, info, INT2VAL(0));
    return;
  }
  
  char *pos = memmem(str, len, match, matchlen);
  if (pos == NULL) {
    vm_return(state, info, INT2VAL(-1));
    return;
  }
  
  int pos_utf8 = utf8_strnlen(str, pos - str);
  vm_return(state, info, INT2VAL(pos_utf8));
}

static void string_byte_len_fn(VMState *state, CallInfo *info) {
  VM_ASSERT(info->args_len == 0, "wrong arity: expected 0, got %i", info->args_len);
  Object *string_base = state->shared->vcache.string_base;
  
  Object *sobj = obj_instance_of(OBJ_OR_NULL(load_arg(state->frame, info->this_arg)), string_base);
  VM_ASSERT(sobj, "internal error: string.endsWith() called on wrong type of object");
  
  char *str = ((StringObject*) sobj)->value;
  vm_return(state, info, INT2VAL(strlen(str)));
}

typedef enum {
  CMP_EQ,
  CMP_LT,
  CMP_GT,
  CMP_LE,
  CMP_GE
} CompareOp;

static void int_cmp_fn(VMState *state, CallInfo *info, CompareOp cmp) {
  VM_ASSERT(info->args_len == 1, "wrong arity: expected 1, got %i", info->args_len);
  
  Value val1 = load_arg(state->frame, info->this_arg);
  Value val2 = load_arg(state->frame, INFO_ARGS_PTR(info)[0]);
  
  VM_ASSERT(IS_INT(val1), "internal error: int compare function called on wrong type of object");
  if (IS_INT(val2)) {
    int i1 = AS_INT(val1), i2 = AS_INT(val2);
    bool res;
    switch (cmp) {
      case CMP_EQ: res = i1 == i2; break;
      case CMP_LT: res = i1 <  i2; break;
      case CMP_GT: res = i1 >  i2; break;
      case CMP_LE: res = i1 <= i2; break;
      case CMP_GE: res = i1 >= i2; break;
      default: abort();
    }
    vm_return(state, info, BOOL2VAL(res));
    return;
  }
  
  if (IS_FLOAT(val2)) {
    float v1 = AS_INT(val1), v2 = AS_FLOAT(val2);
    bool res;
    switch (cmp) {
      case CMP_EQ: res = v1 == v2; break;
      case CMP_LT: res = v1 <  v2; break;
      case CMP_GT: res = v1 >  v2; break;
      case CMP_LE: res = v1 <= v2; break;
      case CMP_GE: res = v1 >= v2; break;
      default: abort();
    }
    vm_return(state, info, BOOL2VAL(res));
    return;
  }
  VM_ASSERT(false, "don't know how to compare int with %s", get_type_info(state, load_arg(state->frame, INFO_ARGS_PTR(info)[0])));
}

static void int_eq_fn(VMState *state, CallInfo *info) {
  int_cmp_fn(state, info, CMP_EQ);
}

static void int_lt_fn(VMState *state, CallInfo *info) {
  int_cmp_fn(state, info, CMP_LT);
}

static void int_gt_fn(VMState *state, CallInfo *info) {
  int_cmp_fn(state, info, CMP_GT);
}

static void int_le_fn(VMState *state, CallInfo *info) {
  int_cmp_fn(state, info, CMP_LE);
}

static void int_ge_fn(VMState *state, CallInfo *info) {
  int_cmp_fn(state, info, CMP_GE);
}

static void float_cmp_fn(VMState *state, CallInfo *info, CompareOp cmp) {
  VM_ASSERT(info->args_len == 1, "wrong arity: expected 1, got %i", info->args_len);
  
  Value val1 = load_arg(state->frame, info->this_arg);
  Value val2 = load_arg(state->frame, INFO_ARGS_PTR(info)[0]);
  VM_ASSERT(IS_FLOAT(val1), "internal error: float compare function called on wrong type of object");
  
  float v1 = AS_FLOAT(val1), v2;
  if (IS_FLOAT(val2)) v2 = AS_FLOAT(val2);
  else if (IS_INT(val2)) v2 = AS_INT(val2);
  else { vm_error(state, "don't know how to compare float with %s", get_type_info(state, val2)); return; }
  
  bool res;
  switch (cmp) {
    case CMP_EQ: res = v1 == v2; break;
    case CMP_LT: res = v1 <  v2; break;
    case CMP_GT: res = v1 >  v2; break;
    case CMP_LE: res = v1 <= v2; break;
    case CMP_GE: res = v1 >= v2; break;
    default: abort();
  }
  vm_return(state, info, BOOL2VAL(res));
}

static void float_eq_fn(VMState *state, CallInfo *info) {
  float_cmp_fn(state, info, CMP_EQ);
}

static void float_lt_fn(VMState *state, CallInfo *info) {
  float_cmp_fn(state, info, CMP_LT);
}

static void float_gt_fn(VMState *state, CallInfo *info) {
  float_cmp_fn(state, info, CMP_GT);
}

static void float_le_fn(VMState *state, CallInfo *info) {
  float_cmp_fn(state, info, CMP_LE);
}

static void float_ge_fn(VMState *state, CallInfo *info) {
  float_cmp_fn(state, info, CMP_GE);
}

static void float_toint_fn(VMState *state, CallInfo *info) {
  VM_ASSERT(info->args_len == 0, "wrong arity: expected 0, got %i", info->args_len);
  Value arg1 = load_arg(state->frame, info->this_arg);
  VM_ASSERT(IS_FLOAT(arg1), "float.toInt called on wrong type of object");
  vm_return(state, info, INT2VAL((int) AS_FLOAT(arg1)));
}

static void ptr_is_null_fn(VMState *state, CallInfo *info) {
  VM_ASSERT(info->args_len == 0, "wrong arity: expected 0, got %i", info->args_len);
  Object *thisptr = OBJ_OR_NULL(load_arg(state->frame, info->this_arg));
  Object *pointer_base = state->shared->vcache.pointer_base;
  VM_ASSERT(thisptr && thisptr->parent == pointer_base, "internal error");
  PointerObject *ptr_obj = (PointerObject*) thisptr;
  vm_return(state, info, BOOL2VAL(ptr_obj->ptr == NULL));
}

static void array_resize_fn(VMState *state, CallInfo *info) {
  VM_ASSERT(info->args_len == 1, "wrong arity: expected 1, got %i", info->args_len);
  Value this_val = load_arg(state->frame, info->this_arg);
  Object *array_base = state->shared->vcache.array_base;
  ArrayObject *arr_obj = (ArrayObject*) obj_instance_of(OBJ_OR_NULL(this_val), array_base);
  Value arg = load_arg(state->frame, INFO_ARGS_PTR(info)[0]);
  VM_ASSERT(IS_INT(arg), "parameter to resize function must be int");
  VM_ASSERT(arr_obj, "internal error: resize called on object that is not an array");
  VM_ASSERT(arr_obj->owned, "cannot resize an array whose memory we don't own!");
  int oldsize = arr_obj->length;
  int newsize = AS_INT(arg);
  VM_ASSERT(newsize >= 0, "bad size: %i", newsize);
  array_resize(state, arr_obj, newsize, true);
  memset(arr_obj->ptr + oldsize, 0, sizeof(Value) * (newsize - oldsize));
  vm_return(state, info, this_val);
}

static void array_push_fn(VMState *state, CallInfo *info) {
  VM_ASSERT(info->args_len == 1, "wrong arity: expected 1, got %i", info->args_len);
  Value this_val = load_arg(state->frame, info->this_arg);
  Object *array_base = state->shared->vcache.array_base;
  ArrayObject *arr_obj = (ArrayObject*) obj_instance_of(OBJ_OR_NULL(this_val), array_base);
  VM_ASSERT(arr_obj, "internal error: push called on object that is not an array");
  VM_ASSERT(arr_obj->owned, "cannot resize an array whose memory we don't own!");
  Value value = load_arg(state->frame, INFO_ARGS_PTR(info)[0]);
  array_resize(state, arr_obj, arr_obj->length + 1, true);
  arr_obj->ptr[arr_obj->length - 1] = value;
  vm_return(state, info, this_val);
}

static void array_pop_fn(VMState *state, CallInfo *info) {
  VM_ASSERT(info->args_len == 0, "wrong arity: expected 0, got %i", info->args_len);
  Object *array_base = state->shared->vcache.array_base;
  ArrayObject *arr_obj = (ArrayObject*) obj_instance_of(OBJ_OR_NULL(load_arg(state->frame, info->this_arg)), array_base);
  VM_ASSERT(arr_obj, "internal error: pop called on object that is not an array");
  VM_ASSERT(arr_obj->owned, "cannot resize an array whose memory we don't own!");
  VM_ASSERT(arr_obj->length, "array underflow");
  Value res = arr_obj->ptr[arr_obj->length - 1];
  array_resize(state, arr_obj, arr_obj->length - 1, true);
  vm_return(state, info, res);
}

static void array_index_fn(VMState *state, CallInfo *info) {
  VM_ASSERT(info->args_len == 1, "wrong arity: expected 1, got %i", info->args_len);
  Object *array_base = state->shared->vcache.array_base;
  ArrayObject *arr_obj = (ArrayObject*) obj_instance_of(OBJ_OR_NULL(load_arg(state->frame, info->this_arg)), array_base);
  Value arg = load_arg(state->frame, INFO_ARGS_PTR(info)[0]);
  if (!IS_INT(arg)) { vm_return(state, info, VNULL); return; }
  VM_ASSERT(arr_obj, "internal error: array '[]' called on object that is not an array");
  int index = AS_INT(arg);
  VM_ASSERT(index >= 0 && index < arr_obj->length, "array index out of bounds!");
  vm_return(state, info, arr_obj->ptr[index]);
}

static void array_index_assign_fn(VMState *state, CallInfo *info) {
  VM_ASSERT(info->args_len == 2, "wrong arity: expected 2, got %i", info->args_len);
  Object *array_base = state->shared->vcache.array_base;
  ArrayObject *arr_obj = (ArrayObject*) obj_instance_of(OBJ_OR_NULL(load_arg(state->frame, info->this_arg)), array_base);
  Value arg = load_arg(state->frame, INFO_ARGS_PTR(info)[0]);
  VM_ASSERT(arr_obj, "internal error: array '[]=' called on object that is not an array");
  VM_ASSERT(IS_INT(arg), "index of array '[]=' must be int");
  int index = AS_INT(arg);
  VM_ASSERT(index >= 0 && index < arr_obj->length, "array index out of bounds!");
  arr_obj->ptr[index] = load_arg(state->frame, INFO_ARGS_PTR(info)[1]);
  vm_return(state, info, VNULL);
}

static void array_join_fn(VMState *state, CallInfo *info) {
  VM_ASSERT(info->args_len == 1, "wrong arity: expected 1, got %i", info->args_len);
  Object *array_base = state->shared->vcache.array_base;
  Object *string_base = state->shared->vcache.string_base;
  ArrayObject *arr_obj = (ArrayObject*) obj_instance_of(OBJ_OR_NULL(load_arg(state->frame, info->this_arg)), array_base);
  StringObject *str_arg = (StringObject*) obj_instance_of(OBJ_OR_NULL(load_arg(state->frame, INFO_ARGS_PTR(info)[0])), string_base);
  VM_ASSERT(arr_obj, "internal error: 'join' called on object that is not an array");
  VM_ASSERT(str_arg, "argument to array.join() must be string");
  
  int joiner_len = strlen(str_arg->value);
  int res_len = 0;
  int *lens = alloca(sizeof(int) * arr_obj->length);
  for (int i = 0; i < arr_obj->length; i++) {
    if (i > 0) res_len += joiner_len;
    Value entry = arr_obj->ptr[i];
    StringObject *entry_str = (StringObject*) obj_instance_of(OBJ_OR_NULL(entry), string_base);
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
    Value entry = arr_obj->ptr[i];
    StringObject *entry_str = (StringObject*) obj_instance_of(OBJ_OR_NULL(entry), string_base);
    // this is safe - we counted up the length above
    // (assuming nobody changes entry_str under us)
    memcpy(res_cur, entry_str->value, lens[i]);
    res_cur += lens[i];
  }
  res_cur[0] = 0;
  // TODO make string constructor that takes ownership of the pointer instead
  vm_return(state, info, make_string(state, res, res_len));
  free(res);
}

static void file_print_fn(VMState *state, CallInfo *info) {
  Object *pointer_base = state->shared->vcache.pointer_base;
  Object *file_base = AS_OBJ(OBJECT_LOOKUP_STRING(state->root, "file", NULL));
  assert(file_base);
  
  VM_ASSERT(obj_instance_of(OBJ_OR_NULL(load_arg(state->frame, info->this_arg)), file_base), "print() called on object that is not a file");
  Object *hdl_obj = AS_OBJ(OBJECT_LOOKUP_STRING(AS_OBJ(load_arg(state->frame, info->this_arg)), "_handle", NULL));
  VM_ASSERT(hdl_obj, "missing _handle!");
  VM_ASSERT(hdl_obj->parent == pointer_base, "_handle must be a pointer!");
  PointerObject *hdl_ptrobj = (PointerObject*) hdl_obj;
  FILE *file = hdl_ptrobj->ptr;
  for (int i = 0; i < info->args_len; ++i) {
    Value arg = load_arg(state->frame, INFO_ARGS_PTR(info)[i]);
    print_recursive(state, file, arg, true);
    if (state->runstate == VM_ERRORED) return;
  }
  fprintf(file, "\n");
  vm_return(state, info, VNULL);
}

static void file_open_fn(VMState *state, CallInfo *info) {
  VM_ASSERT(info->args_len == 2, "wrong arity: expected 2, got %i", info->args_len);
  Object *file_base = AS_OBJ(OBJECT_LOOKUP_STRING(state->root, "file", NULL));
  Object *string_base = state->shared->vcache.string_base;
  assert(file_base);
  
  VM_ASSERT(OBJ_OR_NULL(load_arg(state->frame, info->this_arg)) == file_base, "open() called on object other than file!");
  StringObject *fnobj = (StringObject*) obj_instance_of(OBJ_OR_NULL(load_arg(state->frame, INFO_ARGS_PTR(info)[0])), string_base);
  VM_ASSERT(fnobj, "first parameter to file.open() must be string!");
  StringObject *fmobj = (StringObject*) obj_instance_of(OBJ_OR_NULL(load_arg(state->frame, INFO_ARGS_PTR(info)[1])), string_base);
  VM_ASSERT(fmobj, "second parameter to file.open() must be string!");
  
  FILE *fh = fopen(fnobj->value, fmobj->value);
  if (fh == NULL) {
    VM_ASSERT(false, "file could not be opened: '%s' as '%s': %s", fnobj->value, fmobj->value, strerror(errno));
  }
  Object *file_obj = AS_OBJ(make_object(state, file_base));
  object_set(state, file_obj, "_handle", make_ptr(state, (void*) fh));
  vm_return(state, info, OBJ2VAL(file_obj));
}

static void file_close_fn(VMState *state, CallInfo *info) {
  VM_ASSERT(info->args_len == 0, "wrong arity: expected 0, got %i", info->args_len);
  Value this_val = load_arg(state->frame, info->this_arg);
  Object *file_base = AS_OBJ(OBJECT_LOOKUP_STRING(state->root, "file", NULL));
  Object *pointer_base = state->shared->vcache.pointer_base;
  assert(file_base);
  
  VM_ASSERT(obj_instance_of(OBJ_OR_NULL(this_val), file_base), "close() called on object that is not a file!");
  Object *hdl_obj = AS_OBJ(OBJECT_LOOKUP_STRING(AS_OBJ(this_val), "_handle", NULL));
  VM_ASSERT(hdl_obj, "missing _handle!");
  VM_ASSERT(hdl_obj->parent == pointer_base, "_handle must be a pointer!");
  PointerObject *hdl_ptrobj = (PointerObject*) hdl_obj;
  FILE *file = hdl_ptrobj->ptr;
  fclose(file);
  object_set(state, AS_OBJ(this_val), "_handle", VNULL);
  
  vm_return(state, info, VNULL);
}

static void print_fn(VMState *state, CallInfo *info) {
  for (int i = 0; i < info->args_len; ++i) {
    Value arg = load_arg(state->frame, INFO_ARGS_PTR(info)[i]);
    print_recursive(state, stdout, arg, true);
    if (state->runstate == VM_ERRORED) return;
  }
  fprintf(stdout, "\n");
  vm_return(state, info, VNULL);
}

static void keys_fn(VMState *state, CallInfo *info) {
  VM_ASSERT(info->args_len == 1, "wrong arity: expected 1, got %i", info->args_len);
  int res_len = 0;
  Object *obj = OBJ_OR_NULL(load_arg(state->frame, INFO_ARGS_PTR(info)[0]));
  if (obj) {
    HashTable *tbl = &obj->tbl;
    for (int i = 0; i < tbl->entries_num; ++i) {
      if (tbl->entries_ptr[i].name_ptr) res_len ++;
    }
  }
  Value *res_ptr = malloc(sizeof(Value) * res_len);
  if (obj) {
    int k = 0;
    HashTable *tbl = &obj->tbl;
    for (int i = 0; i < tbl->entries_num; ++i) {
      TableEntry *entry = &tbl->entries_ptr[i];
      if (entry->name_ptr) {
        res_ptr[k++] = make_string(state, entry->name_ptr, entry->name_len);
      }
    }
  }
  vm_return(state, info, make_array(state, res_ptr, res_len, true));
}

static Object *xml_to_object(VMState *state, xmlNode *element, Object *text_node_base, Object *element_node_base) {
  Object *res;
  if (element->type == 1) {
    res = AS_OBJ(make_object(state, element_node_base));
    xmlNode *child = element->children;
    int children_len = 0;
    for (; child; child = child->next) children_len ++;
    Value *children_ptr = malloc(sizeof(Value) * children_len);
    int i = 0;
    for (child = element->children; child; child = child->next) {
      children_ptr[i++] = OBJ2VAL(xml_to_object(state, child, text_node_base, element_node_base));
    }
    Object *attr = AS_OBJ(make_object(state, NULL));
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
      object_set(state, attr, name2, make_string(state, content, strlen(content)));
    }
    // printf("alloc_string(%lu)\n", strlen((char*) element->name));
    object_set(state, res, "nodeName", make_string(state, (char*) element->name, strlen((char*) element->name)));
    object_set(state, res, "attr", OBJ2VAL(attr));
    object_set(state, res, "children", make_array(state, children_ptr, children_len, true));
  } else if (element->type == 3) {
    res = AS_OBJ(make_object(state, text_node_base));
    // printf("alloc_string(%lu)\n", strlen((char*) element->content));
    char *content = (char*) element->content;
    object_set(state, res, "value", make_string(state, content, strlen(content)));
  } else abort();
  return res;
}

static void xml_load_fn(VMState *state, CallInfo *info) {
  VM_ASSERT(info->args_len == 1, "wrong arity: expected 1, got %i", info->args_len);
  Object *root = state->root;
  Object *string_base = state->shared->vcache.string_base;
  Object *xml_base = AS_OBJ(OBJECT_LOOKUP_STRING(root, "xml", NULL));
  Object *text_node_base = AS_OBJ(OBJECT_LOOKUP_STRING(xml_base, "text_node", NULL));
  Object *element_node_base = AS_OBJ(OBJECT_LOOKUP_STRING(xml_base, "element_node", NULL));
  
  StringObject *str_obj = (StringObject*) obj_instance_of(OBJ_OR_NULL(load_arg(state->frame, INFO_ARGS_PTR(info)[0])), string_base);
  VM_ASSERT(str_obj, "parameter to xml.load must be string");
  char *file = str_obj->value;
  
  LIBXML_TEST_VERSION
  
  xmlDoc *doc = xmlReadFile(file, NULL, 0);
  VM_ASSERT(doc != NULL, "cannot read xml file");
  
  xmlNode *root_element = xmlDocGetRootElement(doc);
  
  gc_disable(state);
  vm_return(state, info, OBJ2VAL(xml_to_object(state, root_element, text_node_base, element_node_base)));
  gc_enable(state);
  
  xmlFreeDoc(doc);
  xmlCleanupParser();
}

static void xml_parse_fn(VMState *state, CallInfo *info) {
  VM_ASSERT(info->args_len == 1, "wrong arity: expected 1, got %i", info->args_len);
  Object *root = state->root;
  Object *string_base = state->shared->vcache.string_base;
  Object *xml_base = AS_OBJ(OBJECT_LOOKUP_STRING(root, "xml", NULL));
  Object *text_node_base = AS_OBJ(OBJECT_LOOKUP_STRING(xml_base, "text_node", NULL));
  Object *element_node_base = AS_OBJ(OBJECT_LOOKUP_STRING(xml_base, "element_node", NULL));
  
  StringObject *str_obj = (StringObject*) obj_instance_of(OBJ_OR_NULL(load_arg(state->frame, INFO_ARGS_PTR(info)[0])), string_base);
  VM_ASSERT(str_obj, "parameter to xml.parse must be string");
  char *text = str_obj->value;
  
  LIBXML_TEST_VERSION
  
  xmlDoc *doc = xmlReadMemory(text, strlen(text), NULL, NULL, 0);
  VM_ASSERT(doc != NULL, "failed to parse XML string");
  
  xmlNode *root_element = xmlDocGetRootElement(doc);
  
  gc_disable(state);
  vm_return(state, info, OBJ2VAL(xml_to_object(state, root_element, text_node_base, element_node_base)));
  gc_enable(state);
  
  xmlFreeDoc(doc);
  xmlCleanupParser();
}

static bool xml_node_check_pred(VMState *state, Value node, Value pred)
{
  VMState substate = {0};
  substate.runstate = VM_TERMINATED;
  substate.parent = state;
  substate.root = state->root;
  substate.shared = state->shared;
  
  Value res;
  
  CallInfo *info = alloca(sizeof(CallInfo) + sizeof(Arg));
  info->args_len = 1;
  info->fn = (Arg) { .kind = ARG_VALUE, .value = pred };
  info->this_arg = (Arg) { .kind = ARG_VALUE, .value = VNULL };
  info->target = (WriteArg) { .kind = ARG_POINTER, .pointer = &res };
  INFO_ARGS_PTR(info)[0] = (Arg) { .kind = ARG_VALUE, .value = node };
  
  if (!setup_call(&substate, info)) {
    VM_ASSERT(false, "pred check failure: %s\n", substate.error) false;
  }
  
  vm_update_frame(&substate);
  vm_run(&substate);
  VM_ASSERT(substate.runstate != VM_ERRORED, "pred check failure: %s\n", substate.error) false;
  
  return value_is_truthy(res);
}

static void xml_node_find_recurse(VMState *state, Value node, Value pred, ArrayObject *aobj)
{
  Object *array_base = state->shared->vcache.array_base;
  bool res = xml_node_check_pred(state, node, pred);
  if (state->runstate == VM_ERRORED) return;
  if (res) {
    array_resize(state, aobj, aobj->length + 1, false);
    aobj->ptr[aobj->length - 1] = node;
  }
  
  Object *children_obj = AS_OBJ(OBJECT_LOOKUP_STRING(closest_obj(state, node), "children", NULL));
  VM_ASSERT(children_obj, "missing 'children' property in node");
  ArrayObject *children_aobj = (ArrayObject*) obj_instance_of(children_obj, array_base);
  VM_ASSERT(children_aobj, "'children' property in node is not an array");
  for (int i = 0; i < children_aobj->length; ++i) {
    Value child = children_aobj->ptr[i];
    xml_node_find_recurse(state, child, pred, aobj);
    if (state->runstate == VM_ERRORED) return;
  }
}

static void xml_node_find_array_fn(VMState *state, CallInfo *info) {
  VM_ASSERT(info->args_len == 1, "wrong arity: expected 1, got %i", info->args_len);
  
  ArrayObject *aobj = (ArrayObject*) AS_OBJ(make_array(state, NULL, 0, true));
  gc_disable(state);
  xml_node_find_recurse(state, load_arg(state->frame, info->this_arg), load_arg(state->frame, INFO_ARGS_PTR(info)[0]), aobj);
  array_resize(state, aobj, aobj->length, true);
  vm_return(state, info, OBJ2VAL((Object*) aobj));
  gc_enable(state);
}

static void xml_node_find_by_name_recurse(VMState *state, Value node, char *name, ArrayObject *aobj)
{
  Object *string_base = state->shared->vcache.string_base;
  Object *array_base = state->shared->vcache.array_base;
  Object *node_obj = closest_obj(state, node);
  Value node_type = OBJECT_LOOKUP_STRING(node_obj, "nodeType", NULL);
  VM_ASSERT(IS_INT(node_type), "invalid xml node");
  if (AS_INT(node_type) == 3) return; // text
  VM_ASSERT(AS_INT(node_type), "node is not element");
  
  Value node_name = OBJECT_LOOKUP_STRING(node_obj, "nodeName", NULL);
  
  VM_ASSERT(NOT_NULL(node_name), "missing 'nodeName' property in node");
  StringObject *nodeName_str = (StringObject*) obj_instance_of(OBJ_OR_NULL(node_name), string_base);
  VM_ASSERT(nodeName_str, "'nodeName' must be string");
  if (strcmp(nodeName_str->value, name) == 0) {
    array_resize(state, aobj, aobj->length + 1, false);
    aobj->ptr[aobj->length - 1] = node;
  }
  
  Value children = OBJECT_LOOKUP_STRING(node_obj, "children", NULL);
  VM_ASSERT(NOT_NULL(children), "missing 'children' property in node");
  ArrayObject *children_arr = (ArrayObject*) obj_instance_of(OBJ_OR_NULL(children), array_base);
  VM_ASSERT(children_arr, "'children' property in node is not an array");
  for (int i = 0; i < children_arr->length; ++i) {
    Value child = children_arr->ptr[i];
    xml_node_find_by_name_recurse(state, child, name, aobj);
    if (state->runstate == VM_ERRORED) return;
  }
}

static void xml_node_find_by_name_array_fn(VMState *state, CallInfo *info) {
  VM_ASSERT(info->args_len == 1, "wrong arity: expected 1, got %i", info->args_len);
  Object *string_base = state->shared->vcache.string_base;
  
  StringObject *name_obj = (StringObject*) obj_instance_of(OBJ_OR_NULL(load_arg(state->frame, INFO_ARGS_PTR(info)[0])), string_base);
  VM_ASSERT(name_obj, "parameter to find_array_by_name must be string!");
  
  ArrayObject *aobj = (ArrayObject*) AS_OBJ(make_array(state, NULL, 0, true));
  gc_disable(state);
  xml_node_find_by_name_recurse(state, load_arg(state->frame, info->this_arg), name_obj->value, aobj);
  array_resize(state, aobj, aobj->length, true);
  vm_return(state, info, OBJ2VAL((Object*)aobj));
  gc_enable(state);
}

#include "language.h"

static void require_fn(VMState *state, CallInfo *info) {
  VM_ASSERT(info->args_len == 1, "wrong arity: expected 1, got %i", info->args_len);
  Object *root = state->root;
  Object *string_base = state->shared->vcache.string_base;
  
  StringObject *file_obj = (StringObject*) obj_instance_of(OBJ_OR_NULL(load_arg(state->frame, INFO_ARGS_PTR(info)[0])), string_base);
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
  substate.runstate = VM_TERMINATED;
  substate.parent = state;
  substate.root = state->root;
  substate.shared = state->shared;
  
  Value resval;
  
  CallInfo info2 = {0};
  info2.target = (WriteArg) { .kind = ARG_POINTER, .pointer = &resval };
  call_function(&substate, root, module, &info2);
  vm_update_frame(&substate);
  vm_run(&substate);
  
  if (substate.runstate == VM_ERRORED) {
    state->runstate = VM_ERRORED;
    state->error = my_asprintf("Error during require('%s')\n%s", filename, substate.error);
    state->backtrace = vm_record_backtrace(&substate, &state->backtrace_depth);
    free(substate.backtrace);
    return;
  }
  
  vm_return(state, info, resval);
}

static void freeze_fn(VMState *state, CallInfo *info) {
  VM_ASSERT(info->args_len == 1, "wrong arity: expected 1, got %i", info->args_len);
  VM_ASSERT(NOT_NULL(load_arg(state->frame, INFO_ARGS_PTR(info)[0])), "can't freeze null");
  VM_ASSERT(IS_OBJ(load_arg(state->frame, INFO_ARGS_PTR(info)[0])), "can't freeze primitive (implicitly frozen!)");
  Object *obj = AS_OBJ(load_arg(state->frame, INFO_ARGS_PTR(info)[0]));
  obj->flags |= OBJ_FROZEN;
  vm_return(state, info, VNULL);
}

static void mark_const_fn(VMState *state, CallInfo *info) {
  VM_ASSERT(info->args_len == 1, "wrong arity: expected 1, got %i", info->args_len);
  Object *string_base = state->shared->vcache.string_base;
  StringObject *key_obj = (StringObject*) obj_instance_of(OBJ_OR_NULL(load_arg(state->frame, INFO_ARGS_PTR(info)[0])), string_base);
  VM_ASSERT(key_obj, "argument to _mark_const must be string");
  
  Value thisval = load_arg(state->frame, info->this_arg); // free functions are called on the local scope
  
  char *key_ptr = key_obj->value;
  int key_len = strlen(key_ptr);
  size_t key_hash = hash(key_ptr, key_len);
  
  vm_return(state, info, VNULL);
  
  Object *context = closest_obj(state, thisval);
  
  Object *cur = context;
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

static void obj_keys_fn(VMState *state, CallInfo *info) {
  VM_ASSERT(info->args_len == 1, "wrong arity: expected 1, got %i", info->args_len);
  VM_ASSERT(NOT_NULL(load_arg(state->frame, INFO_ARGS_PTR(info)[0])), "cannot get keys of null");
  Object *obj = OBJ_OR_NULL(load_arg(state->frame, INFO_ARGS_PTR(info)[0]));
  int keys_len = 0;
  Value *keys_ptr = NULL;
  if (obj) {
    keys_len = obj->tbl.entries_stored;
    keys_ptr = malloc(sizeof(Value) * keys_len);
    int k = 0;
    for (int i = 0; i < obj->tbl.entries_num; i++) {
      const char *name_ptr = obj->tbl.entries_ptr[i].name_ptr;
      int name_len = obj->tbl.entries_ptr[i].name_len;
      if (name_ptr) {
        keys_ptr[k++] = make_string(state, name_ptr, name_len);
      }
    }
    assert(k == keys_len);
  }
  vm_return(state, info, make_array(state, keys_ptr, keys_len, true));
}

static void sin_fn(VMState *state, CallInfo *info) {
  VM_ASSERT(info->args_len == 1, "wrong arity: expected 1, got %i", info->args_len);
  Value val = load_arg(state->frame, INFO_ARGS_PTR(info)[0]);
  float f;
  if (IS_FLOAT(val)) f = AS_FLOAT(val);
  else if (IS_INT(val)) f = AS_INT(val);
  else VM_ASSERT(false, "unexpected type for math.sin()");
  vm_return(state, info, FLOAT2VAL(sinf(f)));
}

static void cos_fn(VMState *state, CallInfo *info) {
  VM_ASSERT(info->args_len == 1, "wrong arity: expected 1, got %i", info->args_len);
  Value val = load_arg(state->frame, INFO_ARGS_PTR(info)[0]);
  float f;
  if (IS_FLOAT(val)) f = AS_FLOAT(val);
  else if (IS_INT(val)) f = AS_INT(val);
  else VM_ASSERT(false, "unexpected type for math.cos()");
  vm_return(state, info, FLOAT2VAL(cosf(f)));
}

static void tan_fn(VMState *state, CallInfo *info) {
  VM_ASSERT(info->args_len == 1, "wrong arity: expected 1, got %i", info->args_len);
  Value val = load_arg(state->frame, INFO_ARGS_PTR(info)[0]);
  float f;
  if (IS_FLOAT(val)) f = AS_FLOAT(val);
  else if (IS_INT(val)) f = AS_INT(val);
  else VM_ASSERT(false, "unexpected type for math.tan()");
  vm_return(state, info, FLOAT2VAL(tanf(f)));
}

static void sqrt_fn(VMState *state, CallInfo *info) {
  VM_ASSERT(info->args_len == 1, "wrong arity: expected 1, got %i", info->args_len);
  float f;
  Value val = load_arg(state->frame, INFO_ARGS_PTR(info)[0]);
  if (IS_FLOAT(val)) f = AS_FLOAT(val);
  else if (IS_INT(val)) f = AS_INT(val);
  else VM_ASSERT(false, "unexpected type for math.sqrt()");
  vm_return(state, info, FLOAT2VAL(sqrtf(f)));
}

static void pow_fn(VMState *state, CallInfo *info) {
  VM_ASSERT(info->args_len == 2, "wrong arity: expected 2, got %i", info->args_len);
  Value val1 = load_arg(state->frame, INFO_ARGS_PTR(info)[0]);
  Value val2 = load_arg(state->frame, INFO_ARGS_PTR(info)[1]);
  float a, b;
  if (IS_FLOAT(val1)) a = AS_FLOAT(val1);
  else if (IS_INT(val1)) a = AS_INT(val1);
  else VM_ASSERT(false, "unexpected type for math.pow()");
  if (IS_FLOAT(val2)) b = AS_FLOAT(val2);
  else if (IS_INT(val2)) b = AS_INT(val2);
  else VM_ASSERT(false, "unexpected type for math.pow()");
  vm_return(state, info, FLOAT2VAL(powf(a, b)));
}

static void assert_fn(VMState *state, CallInfo *info) {
  VM_ASSERT(info->args_len == 1 || info->args_len == 2, "wrong arity: expected 1 or 2, got %i", info->args_len);
  bool test = value_is_truthy(load_arg(state->frame, INFO_ARGS_PTR(info)[0]));
  if (info->args_len == 2) {
    Object *string_base = state->shared->vcache.string_base;
    StringObject *msg_obj = (StringObject*) obj_instance_of(OBJ_OR_NULL(load_arg(state->frame, INFO_ARGS_PTR(info)[1])), string_base);
    VM_ASSERT(msg_obj, "second parameter to assert() must be string");
    VM_ASSERT(test, "assert failed: %s", msg_obj->value);
  } else {
    VM_ASSERT(test, "assert failed");
  }
  vm_return(state, info, VNULL);
}

char *get_type_info(VMState *state, Value val) {
  if (IS_NULL(val)) return "null";
  if (IS_INT(val)) return "int";
  if (IS_BOOL(val)) return "bool";
  if (IS_FLOAT(val)) return "float";
  Object *obj = AS_OBJ(val);
  if (!state) return my_asprintf("%p", (void*) obj);
  if (obj == state->shared->vcache.int_base) return "int";
  if (obj == state->shared->vcache.bool_base) return "bool";
  if (obj == state->shared->vcache.float_base) return "float";
  if (obj == state->shared->vcache.closure_base) return "closure";
  if (obj == state->shared->vcache.function_base) return "function";
  if (obj == state->shared->vcache.array_base) return "array";
  if (obj == state->shared->vcache.string_base) return "string";
  if (obj == state->shared->vcache.pointer_base) return "pointer";
  
  if (obj->parent) return get_type_info(state, OBJ2VAL(obj->parent));
  return "unknown";
}

Object *create_root(VMState *state) {
  Object *root = AS_OBJ(make_object(state, NULL));
  
  state->root = root;
  
  GCRootSet pin_root;
  Value rootval = OBJ2VAL(root);
  gc_add_roots(state, &rootval, 1, &pin_root);
  
  Object *function_obj = AS_OBJ(make_object(state, NULL));
  function_obj->flags |= OBJ_NOINHERIT;
  object_set(state, root, "function", OBJ2VAL(function_obj));
  state->shared->vcache.function_base = function_obj;
  object_set(state, function_obj, "apply", make_fn(state, fn_apply_fn));
  
  Object *int_obj = AS_OBJ(make_object(state, NULL));
  int_obj->flags |= OBJ_NOINHERIT;
  object_set(state, root, "int", OBJ2VAL(int_obj));
  object_set(state, int_obj, "+" , make_fn(state, int_add_fn));
  object_set(state, int_obj, "-" , make_fn(state, int_sub_fn));
  object_set(state, int_obj, "*" , make_fn(state, int_mul_fn));
  object_set(state, int_obj, "/" , make_fn(state, int_div_fn));
  object_set(state, int_obj, "|" , make_fn(state, int_bit_or_fn));
  object_set(state, int_obj, "&" , make_fn(state, int_bit_and_fn));
  object_set(state, int_obj, "==", make_fn(state, int_eq_fn));
  object_set(state, int_obj, "<" , make_fn(state, int_lt_fn));
  object_set(state, int_obj, ">" , make_fn(state, int_gt_fn));
  object_set(state, int_obj, "<=", make_fn(state, int_le_fn));
  object_set(state, int_obj, ">=", make_fn(state, int_ge_fn));
  object_set(state, int_obj, "parse" , make_fn(state, int_parse_fn));
  state->shared->vcache.int_base = int_obj;
  int_obj->flags |= OBJ_FROZEN;
  
  Object *float_obj = AS_OBJ(make_object(state, NULL));
  float_obj->flags |= OBJ_NOINHERIT;
  object_set(state, root, "float", OBJ2VAL(float_obj));
  object_set(state, float_obj, "+" , make_fn(state, float_add_fn));
  object_set(state, float_obj, "-" , make_fn(state, float_sub_fn));
  object_set(state, float_obj, "*" , make_fn(state, float_mul_fn));
  object_set(state, float_obj, "/" , make_fn(state, float_div_fn));
  object_set(state, float_obj, "==", make_fn(state, float_eq_fn));
  object_set(state, float_obj, "<" , make_fn(state, float_lt_fn));
  object_set(state, float_obj, ">" , make_fn(state, float_gt_fn));
  object_set(state, float_obj, "<=", make_fn(state, float_le_fn));
  object_set(state, float_obj, ">=", make_fn(state, float_ge_fn));
  object_set(state, float_obj, "toInt" , make_fn(state, float_toint_fn));
  state->shared->vcache.float_base = float_obj;
  float_obj->flags |= OBJ_FROZEN;
  
  Object *closure_obj = AS_OBJ(make_object(state, NULL));
  closure_obj->flags |= OBJ_NOINHERIT;
  object_set(state, root, "closure", OBJ2VAL(closure_obj));
  object_set(state, closure_obj, "apply", make_fn(state, fn_apply_fn));
  state->shared->vcache.closure_base = closure_obj;
  
  Object *bool_obj = AS_OBJ(make_object(state, NULL));
  bool_obj->flags |= OBJ_NOINHERIT;
  object_set(state, root, "bool", OBJ2VAL(bool_obj));
  object_set(state, bool_obj, "!", make_fn(state, bool_not_fn));
  object_set(state, bool_obj, "==", make_fn(state, bool_eq_fn));
  state->shared->vcache.bool_base = bool_obj;
  bool_obj->flags |= OBJ_FROZEN;
  
  object_set(state, root, "true", BOOL2VAL(true));
  object_set(state, root, "false", BOOL2VAL(false));
  
  object_set(state, root, "null", VNULL);
  
  Object *string_obj = AS_OBJ(make_object(state, NULL));
  string_obj->flags |= OBJ_NOINHERIT;
  object_set(state, root, "string", OBJ2VAL(string_obj));
  object_set(state, string_obj, "+", make_fn(state, string_add_fn));
  object_set(state, string_obj, "==", make_fn(state, string_eq_fn));
  object_set(state, string_obj, "startsWith", make_fn(state, string_startswith_fn));
  object_set(state, string_obj, "endsWith", make_fn(state, string_endswith_fn));
  object_set(state, string_obj, "slice", make_fn(state, string_slice_fn));
  object_set(state, string_obj, "find", make_fn(state, string_find_fn));
  object_set(state, string_obj, "byte_len", make_fn(state, string_byte_len_fn));
  state->shared->vcache.string_base = string_obj;
  
  Object *array_obj = AS_OBJ(make_object(state, NULL));
  array_obj->flags |= OBJ_NOINHERIT;
  object_set(state, root, "array", OBJ2VAL(array_obj));
  object_set(state, array_obj, "resize", make_fn(state, array_resize_fn));
  object_set(state, array_obj, "push", make_fn(state, array_push_fn));
  object_set(state, array_obj, "pop", make_fn(state, array_pop_fn));
  object_set(state, array_obj, "[]", make_fn(state, array_index_fn));
  object_set(state, array_obj, "[]=", make_fn(state, array_index_assign_fn));
  object_set(state, array_obj, "join", make_fn(state, array_join_fn));
  state->shared->vcache.array_base = array_obj;
  
  Object *ptr_obj = AS_OBJ(make_object(state, NULL));
  ptr_obj->flags |= OBJ_NOINHERIT;
  object_set(state, root, "pointer", OBJ2VAL(ptr_obj));
  object_set(state, ptr_obj, "null", make_fn(state, ptr_is_null_fn));
  state->shared->vcache.pointer_base = ptr_obj;
  
  object_set(state, root, "keys", make_fn(state, keys_fn));
  
  Object *xml_obj = AS_OBJ(make_object(state, NULL));
  object_set(state, xml_obj, "load", make_fn(state, xml_load_fn));
  object_set(state, xml_obj, "parse", make_fn(state, xml_parse_fn));
  
  Object *node_obj = AS_OBJ(make_object(state, NULL));
  object_set(state, node_obj, "find_array", make_fn(state, xml_node_find_array_fn));
  object_set(state, node_obj, "find_array_by_name", make_fn(state, xml_node_find_by_name_array_fn));
  xml_obj->flags |= OBJ_FROZEN;
  object_set(state, xml_obj, "node", OBJ2VAL(node_obj));
  
  Object *element_node_obj = AS_OBJ(make_object(state, node_obj));
  object_set(state, element_node_obj, "nodeName", make_string_static(state, ""));
  object_set(state, element_node_obj, "nodeType", INT2VAL(1));
  object_set(state, element_node_obj, "attr", make_object(state, NULL));
  object_set(state, element_node_obj, "children", make_array(state, NULL, 0, true));
  object_set(state, xml_obj, "element_node", OBJ2VAL(element_node_obj));
  
  Object *text_node_obj = AS_OBJ(make_object(state, node_obj));
  object_set(state, text_node_obj, "nodeType", INT2VAL(3));
  object_set(state, text_node_obj, "value", VNULL);
  object_set(state, xml_obj, "text_node", OBJ2VAL(text_node_obj));
  
  object_set(state, root, "xml", OBJ2VAL(xml_obj));
  
  Object *file_obj = AS_OBJ(make_object(state, NULL));
  object_set(state, file_obj, "_handle", VNULL);
  object_set(state, file_obj, "print", make_fn(state, file_print_fn));
  object_set(state, file_obj, "open", make_fn(state, file_open_fn));
  object_set(state, file_obj, "close", make_fn(state, file_close_fn));
  file_obj->flags |= OBJ_FROZEN;
  object_set(state, root, "file", OBJ2VAL(file_obj));
  
  Object *stdout_obj = AS_OBJ(make_object(state, file_obj));
  object_set(state, stdout_obj, "_handle", make_ptr(state, (void*) stdout));
  object_set(state, root, "stdout", OBJ2VAL(stdout_obj));
  
  Object *stderr_obj = AS_OBJ(make_object(state, file_obj));
  object_set(state, stderr_obj, "_handle", make_ptr(state, (void*) stderr));
  object_set(state, root, "stderr", OBJ2VAL(stderr_obj));
  
  object_set(state, root, "print", make_fn(state, print_fn));
  
  object_set(state, root, "require", make_fn(state, require_fn));
  object_set(state, root, "freeze", make_fn(state, freeze_fn));
  object_set(state, root, "_mark_const", make_fn(state, mark_const_fn));
  object_set(state, root, "assert", make_fn(state, assert_fn));
  
  Object *math_obj = AS_OBJ(make_object(state, NULL));
  object_set(state, math_obj, "sin", make_fn(state, sin_fn));
  object_set(state, math_obj, "cos", make_fn(state, cos_fn));
  object_set(state, math_obj, "tan", make_fn(state, tan_fn));
  object_set(state, math_obj, "sqrt", make_fn(state, sqrt_fn));
  object_set(state, math_obj, "pow", make_fn(state, pow_fn));
  math_obj->flags |= OBJ_FROZEN;
  object_set(state, root, "math", OBJ2VAL(math_obj));
  
  Object *obj_tools = AS_OBJ(make_object(state, NULL));
  obj_tools->flags |= OBJ_NOINHERIT;
  object_set(state, obj_tools, "keys", make_fn(state, obj_keys_fn));
  
  object_set(state, root, "object", OBJ2VAL(obj_tools));
  
  ffi_setup_root(state, root);
  
  root->flags |= OBJ_FROZEN;
  gc_remove_roots(state, &pin_root);
  
  return root;
}
