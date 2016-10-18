#include "vm/runtime.h"

#include <math.h>
#include <stdio.h>
#include <errno.h>
#include <stdint.h>
#include <unistd.h>

#include "win32_compat.h"

#include "vm/call.h"
#include "vm/ffi.h"
#include "gc.h"
#include "print.h"

#include <libxml/parser.h>
#include <libxml/tree.h>

#ifdef _WIN32
#include <windows.h>
#include <shlwapi.h>
#endif

FnWrap call_internal(VMState *state, CallInfo *info, Instr *instr_after_call);

/* why does 'apply' take 'this' as an explicit first parameter?
 * well, consider foo.bar.apply()
 * by the time apply gets called, its parameter is just 'bar'
 * the lookup on foo is done and gone
 * there is no way to reconstruct 'foo' from apply's position
 * hence, we explicitly pass it in*/
static void fn_apply_fn(VMState *state, CallInfo *info) {
  VM_ASSERT(info->args_len == 2, "wrong arity: expected 2, got %i", info->args_len);
  Value this_value = load_arg(state->frame, INFO_ARGS_PTR(info)[0]);
  Object *array_base = state->shared->vcache.array_base;
  ArrayObject *args_array = (ArrayObject*) obj_instance_of(OBJ_OR_NULL(load_arg(state->frame, INFO_ARGS_PTR(info)[1])), array_base);
  VM_ASSERT(args_array, "argument to apply() must be array!");
  int len = args_array->length;
  Value fn_value = load_arg(state->frame, info->this_arg);
  
  setup_stub_frame(state, len);
  state->frame->target = info->target;
  for (int i = 0; i < len; ++i) {
    state->frame->slots_ptr[i + 1] = args_array->ptr[i];
  }
  
  CallInfo *info2 = alloca(sizeof(CallInfo) + sizeof(Arg) * len);
  info2->args_len = len;
  info2->this_arg = (Arg) { .kind = ARG_VALUE, .value = this_value };
  info2->fn = (Arg) { .kind = ARG_VALUE, .value = fn_value };
  info2->target = (WriteArg) { .kind = ARG_SLOT, .slot = 0 }; // 0 is the return slot
  for (int i = 0; i < len; ++i) {
    INFO_ARGS_PTR(info2)[i] = (Arg) { .kind = ARG_SLOT, .slot = i + 1 };
  }
  // passthrough call to actual function
  // note: may set its own errors
  state->frame->instr_ptr = state->instr;
  call_internal(state, info2, (Instr*)((char*) state->instr + instr_size(state->instr)));
}

// foo.bar.call(xz, 3, 4, 5) -> xz.bar(3, 4, 5) can be done by just rewriting info
static void fn_call_fn(VMState *state, CallInfo *info) {
  CallInfo *info2 = alloca(sizeof(CallInfo) + sizeof(Arg) * (info->args_len - 1));
  info2->args_len = info->args_len - 1;
  info2->fn = info->this_arg;
  info2->target = info->target;
  info2->this_arg = INFO_ARGS_PTR(info)[0];
  for (int i = 1; i < info->args_len; i++) {
    INFO_ARGS_PTR(info2)[i-1] = INFO_ARGS_PTR(info)[i];
  }
  call_internal(state, info2, (Instr*)((char*) state->instr + instr_size(state->instr)));
}

static void bool_eq_fn(VMState *state, CallInfo *info) {
  VM_ASSERT(info->args_len == 1, "wrong arity: expected 1, got %i", info->args_len);
  
  Value val1 = load_arg(state->frame, info->this_arg), val2 = load_arg(state->frame, INFO_ARGS_PTR(info)[0]);
  
  VM_ASSERT(IS_BOOL(val1), "internal error: bool compare function called on wrong type of object");
  VM_ASSERT(IS_BOOL(val2), "can't compare bool with this value");
  vm_return(state, info, BOOL2VAL(AS_BOOL(val1) == AS_BOOL(val2)));
}

typedef enum {
  MATH_ADD,
  MATH_SUB,
  MATH_MUL,
  MATH_DIV,
  MATH_MOD,
  MATH_BIT_OR,
  MATH_BIT_AND
} MathOp;

static inline void int_math_fn(VMState *state, CallInfo *info, MathOp mop) __attribute__ ((always_inline));
static inline void int_math_fn(VMState *state, CallInfo *info, MathOp mop) {
  VM_ASSERT(info->args_len == 1, "wrong arity: expected 1, got %i", info->args_len);
  
  Value this_val = load_arg(state->frame, info->this_arg);
  VM_ASSERT(IS_INT(this_val), "internal error: int math function called on wrong type of object"); // otherwise how are we called on it??
  Value val1 = load_arg(state->frame, info->this_arg);
  Value val2 = load_arg(state->frame, INFO_ARGS_PTR(info)[0]);
  
  if (LIKELY(IS_INT(val2))) { // int math mostly with int
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
      case MATH_MOD:
        VM_ASSERT(i2 != 0, "division by zero");
        res = i1 % i2;
        break;
      case MATH_BIT_OR: res = i1 | i2; break;
      case MATH_BIT_AND: res = i1 & i2; break;
      default: abort();
    }
    vm_return(state, info, INT2VAL(res));
    return;
  }
  
  if (LIKELY(IS_FLOAT(val2))) {
    int i1 = AS_INT(val1); float v2 = AS_FLOAT(val2);
    float res;
    switch (mop) {
      case MATH_ADD: res = i1 + v2; break;
      case MATH_SUB: res = i1 - v2; break;
      case MATH_MUL: res = i1 * v2; break;
      case MATH_MOD:
        VM_ASSERT(v2 > 0.0f, "what are you even doing");
        res = fmodf(i1, v2);
        if (res < 0) res += v2;
        break;
      case MATH_DIV:
        VM_ASSERT(v2 != 0.0f, "float division by zero");
        res = i1 / v2;
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

static void int_mod_fn(VMState *state, CallInfo *info) {
  int_math_fn(state, info, MATH_MOD);
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

static inline void float_math_fn(VMState *state, CallInfo *info, MathOp mop) __attribute__ ((always_inline));
static inline void float_math_fn(VMState *state, CallInfo *info, MathOp mop) {
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
    case MATH_MOD:
      VM_ASSERT(v2 > 0.0f, "what are you even doing");
      res = fmodf(v1, v2);
      if (res < 0) res += v2;
      break;
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

static void float_mod_fn(VMState *state, CallInfo *info) {
  float_math_fn(state, info, MATH_MOD);
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

static void string_replace_fn(VMState *state, CallInfo *info) {
  VM_ASSERT(info->args_len == 2, "wrong arity: expected 2, got %i", info->args_len);
  Object *string_base = state->shared->vcache.string_base;
  
  StringObject *sobj = (StringObject*) obj_instance_of(OBJ_OR_NULL(load_arg(state->frame, info->this_arg)), string_base);
  VM_ASSERT(sobj, "internal error: string.replace() called on wrong type of object");
  StringObject *sobj2 = (StringObject*) obj_instance_of(OBJ_OR_NULL(load_arg(state->frame, INFO_ARGS_PTR(info)[0])), string_base);
  VM_ASSERT(sobj2, "internal error: string.replace() expects string as first arg");
  StringObject *sobj3 = (StringObject*) obj_instance_of(OBJ_OR_NULL(load_arg(state->frame, INFO_ARGS_PTR(info)[1])), string_base);
  VM_ASSERT(sobj3, "internal error: string.replace() expects string as second arg");
  
  char *str = sobj->value;
  int len = strlen(str);
  char *match = sobj2->value;
  int matchlen = strlen(match);
  char *subst = sobj3->value;
  int substlen = strlen(subst);
  if (matchlen == 0 || substlen == 0) {
    vm_return(state, info, OBJ2VAL((Object*)sobj));
    return;
  }
  
  char *res = NULL;
  int reslen = 0;
  while (true) {
    char *pos = memmem(str, len, match, matchlen);
    if (pos == NULL) {
      res = realloc(res, reslen + len);
      memcpy(res + reslen, str, len);
      reslen += len;
      vm_return(state, info, make_string(state, res, reslen));
      return;
    }
    int ipos = pos - str;
    res = realloc(res, reslen + ipos + substlen);
    memcpy(res + reslen, str, ipos);
    memcpy(res + reslen + ipos, subst, substlen);
    reslen += ipos + substlen;
    str += ipos + matchlen;
  }
  abort();
}

static void string_byte_len_fn(VMState *state, CallInfo *info) {
  VM_ASSERT(info->args_len == 0, "wrong arity: expected 0, got %i", info->args_len);
  Object *string_base = state->shared->vcache.string_base;
  
  Object *sobj = obj_instance_of(OBJ_OR_NULL(load_arg(state->frame, info->this_arg)), string_base);
  VM_ASSERT(sobj, "internal error: string.byte_len() called on wrong type of object");
  
  char *str = ((StringObject*) sobj)->value;
  vm_return(state, info, INT2VAL(strlen(str)));
}

#include "vm/instrs/int_cmp.h"

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

#include "vm/instrs/int_eq.h"

#define LHS_KIND ARG_SLOT
#define RHS_KIND ARG_SLOT
#define TARGET_KIND ARG_SLOT
#define FN_NAME int_eq_fn_ls_rs_ts
#define FAST_FN_NAME int_eq_fn_ls_rs_ts_fast
  #include "vm/instrs/int_cmp.h"
  #include "vm/instrs/int_eq.h"
#undef LHS_KIND
#undef RHS_KIND
#undef TARGET_KIND
#undef FN_NAME
#undef FAST_FN_NAME

#define LHS_KIND ARG_REFSLOT
#define RHS_KIND ARG_VALUE
#define TARGET_KIND ARG_SLOT
#define FN_NAME int_eq_fn_lr_rv_ts
#define FAST_FN_NAME int_eq_fn_lr_rv_ts_fast
  #include "vm/instrs/int_cmp.h"
  #include "vm/instrs/int_eq.h"
#undef LHS_KIND
#undef RHS_KIND
#undef TARGET_KIND
#undef FN_NAME
#undef FAST_FN_NAME

FnWrap int_eq_fn_dispatch(Instr *instr) {
  assert(instr->type == INSTR_CALL_FUNCTION_DIRECT);
  CallFunctionDirectInstr *cfdi = (CallFunctionDirectInstr*) instr;
  CallInfo *info = &cfdi->info;
  if (cfdi->info.this_arg.kind == ARG_REFSLOT
    && INFO_ARGS_PTR(info)[0].kind == ARG_VALUE
    && cfdi->info.target.kind == ARG_SLOT
  ) {
    return (FnWrap) { &int_eq_fn_lr_rv_ts_fast };
  }
  if (cfdi->info.this_arg.kind == ARG_SLOT
    && INFO_ARGS_PTR(info)[0].kind == ARG_SLOT
    && cfdi->info.target.kind == ARG_SLOT
  ) {
    return (FnWrap) { &int_eq_fn_ls_rs_ts_fast };
  }
  return (FnWrap) { &int_eq_fn_fast };
}

static inline void float_cmp_fn(VMState *state, CallInfo *info, CompareOp cmp) __attribute__ ((always_inline));
static inline void float_cmp_fn(VMState *state, CallInfo *info, CompareOp cmp) {
  VM_ASSERT(info->args_len == 1, "wrong arity: expected 1, got %i", info->args_len);
  
  Value val1 = load_arg(state->frame, info->this_arg);
  Value val2 = load_arg(state->frame, INFO_ARGS_PTR(info)[0]);
  VM_ASSERT(IS_FLOAT(val1), "internal error: float compare function called on wrong type of object");
  
  float v1 = AS_FLOAT(val1), v2;
  if (IS_FLOAT(val2)) v2 = AS_FLOAT(val2);
  else if (LIKELY(IS_INT(val2))) v2 = AS_INT(val2);
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
  if (newsize > oldsize) memset(arr_obj->ptr + oldsize, 0, sizeof(Value) * (newsize - oldsize));
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
  VM_ASSERT(arr_obj, "internal error: array '[]' called on object that is not an array");
  Value arg = load_arg(state->frame, INFO_ARGS_PTR(info)[0]);
  if (!IS_INT(arg)) {
    VM_ASSERT(false, "No such key in array object.");
  }
  int index = AS_INT(arg);
  VM_ASSERT(index >= 0 && index < arr_obj->length, "array index out of bounds!");
  vm_return(state, info, arr_obj->ptr[index]);
}

static void array_iterator_next_fn(VMState *state, CallInfo *info) {
  VM_ASSERT(info->args_len == 0, "wrong arity: expected 0, got %i", info->args_len);
  Object *this_obj = OBJ_OR_NULL(load_arg(state->frame, info->this_arg));
  VM_ASSERT(this_obj, "internal error");
  
  Object *array_base = state->shared->vcache.array_base;
  ArrayObject *arr_obj = (ArrayObject*) obj_instance_of(OBJ_OR_NULL(OBJECT_LOOKUP_STRING(this_obj, "array", NULL)), array_base);
  VM_ASSERT(arr_obj, "internal error");
  
  Value index_val = OBJECT_LOOKUP_STRING(this_obj, "index", NULL);
  VM_ASSERT(IS_INT(index_val), "internal error");
  
  Object *iter_obj = AS_OBJ(make_object(state, NULL, false));
  
  int index = AS_INT(index_val);
  if (index >= arr_obj->length) {
    OBJECT_SET_STRING(state, iter_obj, "done", BOOL2VAL(true));
  } else {
    OBJECT_SET_STRING(state, iter_obj, "done", BOOL2VAL(false));
    OBJECT_SET_STRING(state, iter_obj, "key", index_val);
    OBJECT_SET_STRING(state, iter_obj, "value", arr_obj->ptr[AS_INT(index_val)]);
  }
  OBJECT_SET_STRING(state, this_obj, "index", INT2VAL(index + 1));
  
  vm_return(state, info, OBJ2VAL(iter_obj));
}

static void array_iterator_fn(VMState *state, CallInfo *info) {
  VM_ASSERT(info->args_len == 0, "wrong arity: expected 0, got %i", info->args_len);
  Object *array_base = state->shared->vcache.array_base;
  ArrayObject *arr_obj = (ArrayObject*) obj_instance_of(OBJ_OR_NULL(load_arg(state->frame, info->this_arg)), array_base);
  VM_ASSERT(arr_obj, "internal error: array iterator called on object that is not an array");
  Object *iterator = AS_OBJ(make_object(state, NULL, false));
  OBJECT_SET_STRING(state, iterator, "array", OBJ2VAL((Object*) arr_obj));
  OBJECT_SET_STRING(state, iterator, "index", INT2VAL(0));
  OBJECT_SET_STRING(state, iterator, "next", make_fn(state, array_iterator_next_fn));
  
  vm_return(state, info, OBJ2VAL(iterator));
}

static void array_in_fn(VMState *state, CallInfo *info) {
  VM_ASSERT(info->args_len == 1, "wrong arity: expected 1, got %i", info->args_len);
  Object *array_base = state->shared->vcache.array_base;
  ArrayObject *arr_obj = (ArrayObject*) obj_instance_of(OBJ_OR_NULL(load_arg(state->frame, info->this_arg)), array_base);
  VM_ASSERT(arr_obj, "internal error: array 'in' overload called on object that is not an array");
  Value arg = load_arg(state->frame, INFO_ARGS_PTR(info)[0]);
  if (!IS_INT(arg)) {
    vm_return(state, info, BOOL2VAL(false));
    return;
  }
  int index = AS_INT(arg);
  vm_return(state, info, BOOL2VAL(index >= 0 && index < arr_obj->length));
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

// TODO runtime eq(a, b) function
static void array_compare_fn(VMState *state, CallInfo *info) {
  VM_ASSERT(info->args_len == 1, "wrong arity: expected 1, got %i", info->args_len);
  Object *array_base = state->shared->vcache.array_base;
  ArrayObject *arr_obj1 = (ArrayObject*) obj_instance_of(OBJ_OR_NULL(load_arg(state->frame, info->this_arg)), array_base);
  ArrayObject *arr_obj2 = (ArrayObject*) obj_instance_of(OBJ_OR_NULL(load_arg(state->frame, INFO_ARGS_PTR(info)[0])), array_base);
  VM_ASSERT(arr_obj1, "internal error: array '==' called on object that is not an array");
  VM_ASSERT(arr_obj2, "internal error: right-side argument to array '==' is not an array");
  bool res = true;
  if (arr_obj1->length != arr_obj2->length) res = false;
  else {
    for (int i = 0; i < arr_obj1->length; i++) {
      // TODO recurse
      Value val1 = arr_obj1->ptr[i], val2 = arr_obj2->ptr[i];
      if (val1.type != val2.type) res = false;
      else if (val1.type == TYPE_NULL) res = true;
      else if (val1.type == TYPE_OBJECT) {
        res = val1.obj == val2.obj;
      } else if (val1.type == TYPE_BOOL) {
        res = val1.b == val2.b;
      } else if (val1.type == TYPE_INT) {
        res = val1.i == val2.i;
      } else if (val1.type == TYPE_FLOAT) {
        res = val1.f == val2.f;
      } else assert(false);
    }
  }
  vm_return(state, info, BOOL2VAL(res));
}

static void array_splice_fn(VMState *state, CallInfo *info) {
  VM_ASSERT(info->args_len >= 1, "wrong arity: expected 1 or more, got %i", info->args_len);
  Object *array_base = state->shared->vcache.array_base;
  ArrayObject *arr_obj = (ArrayObject*) obj_instance_of(OBJ_OR_NULL(load_arg(state->frame, info->this_arg)), array_base);
  VM_ASSERT(arr_obj, "internal error: array 'splice()' called on object that is not an array");
  int len = arr_obj->length;
  
  Value start_val = load_arg(state->frame, INFO_ARGS_PTR(info)[0]);
  VM_ASSERT(IS_INT(start_val), "array 'splice()' called with non-int");
  int start = AS_INT(start_val);
  VM_ASSERT(start >= 0 && start <= len, "start out of bounds");
  
  int deleteCount = len - start;
  if (info->args_len > 1) {
    Value deleteCount_val = load_arg(state->frame, INFO_ARGS_PTR(info)[1]);
    VM_ASSERT(IS_INT(deleteCount_val), "array 'splice()' called with non-int argument 2");
    deleteCount = AS_INT(deleteCount_val);
  }
  VM_ASSERT(deleteCount >= 0, "deleteCount out of bounds");
  if (start + deleteCount > len) deleteCount = len - start;
  
  int new_items = info->args_len - 2;
  new_items = (new_items >= 0) ? new_items : 0;
  
  int newlen = len - deleteCount + new_items;
  int kept_right_items = len - start - deleteCount;
  
  if (new_items > deleteCount) {
    array_resize(state, arr_obj, newlen, true);
    // shift up
    // 2, 3, 4: .slice(1, 1, 6, 7) => 2, 6, 7, 4
    for (int i = 0; i < kept_right_items; i++) {
      arr_obj->ptr[newlen - 1 - i] = arr_obj->ptr[len - 1 - i];
    }
  }
  for (int i = 0; i < new_items; i++) {
    arr_obj->ptr[start + i] = load_arg(state->frame, INFO_ARGS_PTR(info)[2 + i]);
  }
  if (new_items < deleteCount) {
    // shift down
    // 1, 2, 3, 4: .slice(1, 2, 6) => 1, 6, 4
    for (int i = 0; i < kept_right_items; i++) {
      arr_obj->ptr[start + new_items + i] = arr_obj->ptr[start + deleteCount + i];
    }
    array_resize(state, arr_obj, newlen, true);
  }
  vm_return(state, info, OBJ2VAL((Object*) arr_obj));
}

static void array_dup_fn(VMState *state, CallInfo *info) {
  VM_ASSERT(info->args_len == 0, "wrong arity: expected 0, got %i", info->args_len);
  Object *array_base = state->shared->vcache.array_base;
  ArrayObject *arr_obj = (ArrayObject*) obj_instance_of(OBJ_OR_NULL(load_arg(state->frame, info->this_arg)), array_base);
  VM_ASSERT(arr_obj, "internal error: array 'splice()' called on object that is not an array");
  
  ArrayObject *new_arr = (ArrayObject*) AS_OBJ(make_array(state, NULL, 0, true));
  array_resize(state, new_arr, arr_obj->length, true);
  if (arr_obj->length) memcpy(new_arr->ptr, arr_obj->ptr, sizeof(Value) * arr_obj->length);
  vm_return(state, info, OBJ2VAL((Object*) new_arr));
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

static void file_exists_fn(VMState *state, CallInfo *info) {
  VM_ASSERT(info->args_len == 1, "wrong arity: expected 1, got %i", info->args_len);
  Object *file_base = AS_OBJ(OBJECT_LOOKUP_STRING(state->root, "file", NULL));
  Object *string_base = state->shared->vcache.string_base;
  assert(file_base);
  
  VM_ASSERT(OBJ_OR_NULL(load_arg(state->frame, info->this_arg)) == file_base, "exists() called on object other than file!");
  StringObject *fnobj = (StringObject*) obj_instance_of(OBJ_OR_NULL(load_arg(state->frame, INFO_ARGS_PTR(info)[0])), string_base);
  VM_ASSERT(fnobj, "first parameter to file.exists() must be string!");
  
  bool validfile = access(fnobj->value, F_OK) != -1;
  vm_return(state, info, BOOL2VAL(validfile));
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
  Object *file_obj = AS_OBJ(make_object(state, file_base, false));
  OBJECT_SET_STRING(state, file_obj, "_handle", make_ptr(state, (void*) fh));
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
  OBJECT_SET_STRING(state, AS_OBJ(this_val), "_handle", VNULL);
  
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
      if (tbl->entries_ptr[i].key_ptr) res_len ++;
    }
  }
  Value *res_ptr = malloc(sizeof(Value) * res_len);
  if (obj) {
    int k = 0;
    HashTable *tbl = &obj->tbl;
    for (int i = 0; i < tbl->entries_num; ++i) {
      TableEntry *entry = &tbl->entries_ptr[i];
      if (entry->key_ptr) {
        res_ptr[k++] = make_string(state, entry->key_ptr, strlen(entry->key_ptr));
      }
    }
  }
  vm_return(state, info, make_array(state, res_ptr, res_len, true));
}

static Object *xml_to_object(VMState *state, Object *parent, xmlNode *element, Object *text_node_base, Object *element_node_base) {
  Object *res;
  if (element->type == 1) {
    res = AS_OBJ(make_object(state, element_node_base, false));
    xmlNode *child = element->children;
    int children_len = 0;
    for (; child; child = child->next) children_len ++;
    Value *children_ptr = malloc(sizeof(Value) * children_len);
    int i = 0;
    for (child = element->children; child; child = child->next) {
      children_ptr[i++] = OBJ2VAL(xml_to_object(state, res, child, text_node_base, element_node_base));
    }
    Object *attr = AS_OBJ(make_object(state, NULL, false));
    xmlAttr *xml_attr = element->properties;
    for (; xml_attr; xml_attr = xml_attr->next) {
      assert(xml_attr->type == 2); // attribute
      assert(xml_attr->children && xml_attr->children->type == 3); // text (TODO handle other types?)
      FastKey name_key = prepare_key((char*) xml_attr->name, strlen((char*) xml_attr->name));
      // printf("alloc_string(%lu)\n", strlen((char*) xml_attr->children->content));
      char *content = (char*) xml_attr->children->content;
      object_set(state, attr, &name_key, make_string(state, content, strlen(content)));
    }
    // printf("alloc_string(%lu)\n", strlen((char*) element->name));
    OBJECT_SET_STRING(state, res, "nodeName", make_string(state, (char*) element->name, strlen((char*) element->name)));
    OBJECT_SET_STRING(state, res, "attr", OBJ2VAL(attr));
    OBJECT_SET_STRING(state, res, "children", make_array(state, children_ptr, children_len, true));
    // otherwise VNULL via prototype
    if (parent) OBJECT_SET_STRING(state, res, "parent", OBJ2VAL(parent));
  } else if (element->type == 3) {
    res = AS_OBJ(make_object(state, text_node_base, false));
    // printf("alloc_string(%lu)\n", strlen((char*) element->content));
    char *content = (char*) element->content;
    OBJECT_SET_STRING(state, res, "value", make_string(state, content, strlen(content)));
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
  vm_return(state, info, OBJ2VAL(xml_to_object(state, NULL, root_element, text_node_base, element_node_base)));
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
  vm_return(state, info, OBJ2VAL(xml_to_object(state, NULL, root_element, text_node_base, element_node_base)));
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

static void xml_node_find_recurse(VMState *state, Value node, Value pred, ArrayObject *aobj, Object *element_node_obj)
{
  Object *array_base = state->shared->vcache.array_base;
  bool res = xml_node_check_pred(state, node, pred);
  if (state->runstate == VM_ERRORED) return;
  if (res) {
    array_resize(state, aobj, aobj->length + 1, false);
    aobj->ptr[aobj->length - 1] = node;
  }
  
  if (obj_instance_of(closest_obj(state, node), element_node_obj)) {
    Object *children_obj = AS_OBJ(OBJECT_LOOKUP_STRING(closest_obj(state, node), "children", NULL));
    VM_ASSERT(children_obj, "missing 'children' property in node");
    ArrayObject *children_aobj = (ArrayObject*) obj_instance_of(children_obj, array_base);
    VM_ASSERT(children_aobj, "'children' property in node is not an array");
    for (int i = 0; i < children_aobj->length; ++i) {
      Value child = children_aobj->ptr[i];
      xml_node_find_recurse(state, child, pred, aobj, element_node_obj);
      if (state->runstate == VM_ERRORED) return;
    }
  }
}

static void xml_node_find_array_fn(VMState *state, CallInfo *info) {
  VM_ASSERT(info->args_len == 1, "wrong arity: expected 1, got %i", info->args_len);
  
  ArrayObject *aobj = (ArrayObject*) AS_OBJ(make_array(state, NULL, 0, true));
  Object *xml_base = AS_OBJ(OBJECT_LOOKUP_STRING(state->root, "xml", NULL));
  Object *elembase = AS_OBJ(OBJECT_LOOKUP_STRING(xml_base, "element_node", NULL));
  gc_disable(state);
  xml_node_find_recurse(state, load_arg(state->frame, info->this_arg), load_arg(state->frame, INFO_ARGS_PTR(info)[0]), aobj, elembase);
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
  array_resize(state, aobj, aobj->length, true); // set length
  vm_return(state, info, OBJ2VAL((Object*)aobj));
  gc_enable(state);
}

static bool values_identical_plus_str(VMState *state, Value v1, Value v2) {
  if (LIKELY(IS_OBJ(v1) && IS_OBJ(v2))) {
    Object *string_base = state->shared->vcache.string_base;
    StringObject *str1 = (StringObject*) obj_instance_of(AS_OBJ(v1), string_base);
    StringObject *str2 = (StringObject*) obj_instance_of(AS_OBJ(v2), string_base);
    return strcmp(str1->value, str2->value) == 0;
  }
  return values_identical(v1, v2);
}

static void xml_node_find_by_attr_recurse(VMState *state, Value node, char *attr, Value value, ArrayObject *aobj)
{
  Object *array_base = state->shared->vcache.array_base;
  Object *node_obj = closest_obj(state, node);
  Value node_type = OBJECT_LOOKUP_STRING(node_obj, "nodeType", NULL);
  VM_ASSERT(IS_INT(node_type), "invalid xml node");
  if (AS_INT(node_type) == 3) return; // text
  VM_ASSERT(AS_INT(node_type), "node is not element");
  
  Object *attr_obj = closest_obj(state, OBJECT_LOOKUP_STRING(node_obj, "attr", NULL));
  VM_ASSERT(attr_obj, "attr property must not be null");
  
  bool entry_found = false;
  Value attr_entry = OBJECT_LOOKUP_STRING(attr_obj, attr, &entry_found);
  // TODO is handling the "=" overload necessary here? I think not?
  if (entry_found && values_identical_plus_str(state, attr_entry, value)) {
    array_resize(state, aobj, aobj->length + 1, false);
    aobj->ptr[aobj->length - 1] = node;
  }
  
  Value children = OBJECT_LOOKUP_STRING(node_obj, "children", NULL);
  VM_ASSERT(NOT_NULL(children), "missing 'children' property in node");
  ArrayObject *children_arr = (ArrayObject*) obj_instance_of(OBJ_OR_NULL(children), array_base);
  VM_ASSERT(children_arr, "'children' property in node is not an array");
  for (int i = 0; i < children_arr->length; ++i) {
    Value child = children_arr->ptr[i];
    xml_node_find_by_attr_recurse(state, child, attr, value, aobj);
    if (state->runstate == VM_ERRORED) return;
  }
}

static void xml_node_find_by_attr_array_fn(VMState *state, CallInfo *info) {
  VM_ASSERT(info->args_len == 2, "wrong arity: expected 2, got %i", info->args_len);
  Object *string_base = state->shared->vcache.string_base;
  
  StringObject *key_obj = (StringObject*) obj_instance_of(OBJ_OR_NULL(load_arg(state->frame, INFO_ARGS_PTR(info)[0])), string_base);
  VM_ASSERT(key_obj, "first parameter to find_array_by_attr must be string!");
  Value cmp_value = load_arg(state->frame, INFO_ARGS_PTR(info)[1]);
  
  ArrayObject *aobj = (ArrayObject*) AS_OBJ(make_array(state, NULL, 0, true));
  gc_disable(state);
  xml_node_find_by_attr_recurse(state, load_arg(state->frame, info->this_arg), key_obj->value, cmp_value, aobj);
  array_resize(state, aobj, aobj->length, true); // set length
  vm_return(state, info, OBJ2VAL((Object*)aobj));
  gc_enable(state);
}

#include "language.h"

typedef struct _ModuleCache ModuleCache;
struct _ModuleCache {
  ModuleCache *next;
  char *filename;
  Value importval;
  GCRootSet my_set;
};

static ModuleCache *mod_cache = 0;

static char *find_file_in_searchpath(VMState *state, char *filename) {
  Object *searchpath = OBJ_OR_NULL(OBJECT_LOOKUP_STRING(state->root, "searchpath", NULL));
  VM_ASSERT(searchpath, "search path must exist, internal error") NULL;
  Object *array_base = state->shared->vcache.array_base;
  Object *string_base = state->shared->vcache.string_base;
  
  ArrayObject *searchpath_array = (ArrayObject*) obj_instance_of(searchpath, array_base);
  VM_ASSERT(searchpath_array, "search path must be array object, internal error") NULL;
  for (int i = 0; i < searchpath_array->length; i++) {
    Object *entry_obj = OBJ_OR_NULL(searchpath_array->ptr[i]);
    StringObject *entry_str = (StringObject*) obj_instance_of(entry_obj, string_base);
    VM_ASSERT(entry_str, "search path entry %i must be string", i) NULL;
    char *path = my_asprintf("%s/%s", entry_str->value, filename);
    if (file_exists(path)) {
      return path;
    }
    free(path);
  }
  return NULL;
}

static void require_fn(VMState *state, CallInfo *info) {
  VM_ASSERT(info->args_len == 1, "wrong arity: expected 1, got %i", info->args_len);
  Object *root = state->root;
  Object *string_base = state->shared->vcache.string_base;
  
  StringObject *file_obj = (StringObject*) obj_instance_of(OBJ_OR_NULL(load_arg(state->frame, INFO_ARGS_PTR(info)[0])), string_base);
  VM_ASSERT(file_obj, "parameter to require() must be string!");
  
  char *filename = file_obj->value;
  filename = find_file_in_searchpath(state, filename);
  if (!filename) return; // asserted
  
  ModuleCache *cur_cache = mod_cache;
  while (cur_cache) {
    if (strcmp(cur_cache->filename, filename) == 0) {
      vm_return(state, info, cur_cache->importval);
      return;
    }
    cur_cache = cur_cache->next;
  }
  
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
  
  CallInfo info2 = {{0}};
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
  
  free_function(module);
  
  ModuleCache *new_mod_cache = malloc(sizeof(ModuleCache));
  *new_mod_cache = (ModuleCache) {
    .next = mod_cache,
    .filename = my_asprintf("%s", filename), // avoid gc on the string object
    .importval = resval
  };
  mod_cache = new_mod_cache;
  // don't accidentally free the module in gc
  // TODO single root set for all cache?
  gc_add_roots(state, &mod_cache->importval, 1, &mod_cache->my_set);
  
  vm_return(state, info, resval);
}

static void freeze_fn(VMState *state, CallInfo *info) {
  VM_ASSERT(info->args_len == 1, "wrong arity: expected 1, got %i", info->args_len);
  Value arg = load_arg(state->frame, INFO_ARGS_PTR(info)[0]);
  VM_ASSERT(NOT_NULL(arg), "can't freeze null");
  if (IS_OBJ(arg)) { // else implicitly frozen - no-op
    Object *obj = AS_OBJ(arg);
    obj->flags |= OBJ_FROZEN;
  }
  vm_return(state, info, VNULL);
}

static void close_fn(VMState *state, CallInfo *info) {
  VM_ASSERT(info->args_len == 1, "wrong arity: expected 1, got %i", info->args_len);
  Value arg = load_arg(state->frame, INFO_ARGS_PTR(info)[0]);
  VM_ASSERT(NOT_NULL(arg), "can't close null");
  if (IS_OBJ(arg)) { // else implicitly closed - no-op
    Object *obj = AS_OBJ(arg);
    obj->flags |= OBJ_CLOSED;
  }
  vm_return(state, info, VNULL);
}

static void mark_const_fn(VMState *state, CallInfo *info) {
  VM_ASSERT(info->args_len == 1, "wrong arity: expected 1, got %i", info->args_len);
  Object *string_base = state->shared->vcache.string_base;
  StringObject *key_obj = (StringObject*) obj_instance_of(OBJ_OR_NULL(load_arg(state->frame, INFO_ARGS_PTR(info)[0])), string_base);
  VM_ASSERT(key_obj, "argument to _mark_const must be string");
  
  Value thisval = load_arg(state->frame, info->this_arg); // free functions are called on the local scope
  
  FastKey key = prepare_key(key_obj->value, strlen(key_obj->value));
  
  vm_return(state, info, VNULL);
  
  Object *context = closest_obj(state, thisval);
  
  Object *cur = context;
  while (cur) {
    TableEntry *entry = table_lookup_prepared(&cur->tbl, &key);
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
      const char *name_ptr = obj->tbl.entries_ptr[i].key_ptr;
      if (name_ptr) {
        int name_len = strlen(name_ptr);
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
  if (LIKELY(IS_FLOAT(val))) f = AS_FLOAT(val);
  else if (IS_INT(val)) f = AS_INT(val);
  else VM_ASSERT(false, "unexpected type for Math.sin()");
  vm_return(state, info, FLOAT2VAL(sinf(f)));
}

static void cos_fn(VMState *state, CallInfo *info) {
  VM_ASSERT(info->args_len == 1, "wrong arity: expected 1, got %i", info->args_len);
  Value val = load_arg(state->frame, INFO_ARGS_PTR(info)[0]);
  float f;
  if (LIKELY(IS_FLOAT(val))) f = AS_FLOAT(val);
  else if (IS_INT(val)) f = AS_INT(val);
  else VM_ASSERT(false, "unexpected type for Math.cos()");
  vm_return(state, info, FLOAT2VAL(cosf(f)));
}

static void tan_fn(VMState *state, CallInfo *info) {
  VM_ASSERT(info->args_len == 1, "wrong arity: expected 1, got %i", info->args_len);
  Value val = load_arg(state->frame, INFO_ARGS_PTR(info)[0]);
  float f;
  if (LIKELY(IS_FLOAT(val))) f = AS_FLOAT(val);
  else if (IS_INT(val)) f = AS_INT(val);
  else VM_ASSERT(false, "unexpected type for Math.tan()");
  vm_return(state, info, FLOAT2VAL(tanf(f)));
}

static void log_fn(VMState *state, CallInfo *info) {
  VM_ASSERT(info->args_len == 1, "wrong arity: expected 1, got %i", info->args_len);
  Value val = load_arg(state->frame, INFO_ARGS_PTR(info)[0]);
  float f;
  if (LIKELY(IS_FLOAT(val))) f = AS_FLOAT(val);
  else if (IS_INT(val)) f = AS_INT(val);
  else VM_ASSERT(false, "unexpected type for Math.tan()");
  vm_return(state, info, FLOAT2VAL(logf(f)));
}

static void sqrt_fn(VMState *state, CallInfo *info) {
  VM_ASSERT(info->args_len == 1, "wrong arity: expected 1, got %i", info->args_len);
  float f;
  Value val = load_arg(state->frame, INFO_ARGS_PTR(info)[0]);
  if (LIKELY(IS_FLOAT(val))) f = AS_FLOAT(val);
  else if (IS_INT(val)) f = AS_INT(val);
  else VM_ASSERT(false, "unexpected type for Math.sqrt()");
  vm_return(state, info, FLOAT2VAL(sqrtf(f)));
}

static void pow_fn(VMState *state, CallInfo *info) {
  VM_ASSERT(info->args_len == 2, "wrong arity: expected 2, got %i", info->args_len);
  Value val1 = load_arg(state->frame, INFO_ARGS_PTR(info)[0]);
  Value val2 = load_arg(state->frame, INFO_ARGS_PTR(info)[1]);
  float a, b;
  if (LIKELY(IS_FLOAT(val1))) a = AS_FLOAT(val1);
  else if (IS_INT(val1)) a = AS_INT(val1);
  else VM_ASSERT(false, "unexpected type for Math.pow()");
  if (IS_FLOAT(val2)) b = AS_FLOAT(val2);
  else if (IS_INT(val2)) b = AS_INT(val2);
  else VM_ASSERT(false, "unexpected type for Math.pow()");
  vm_return(state, info, FLOAT2VAL(powf(a, b)));
}

static void max_fn(VMState *state, CallInfo *info) {
  VM_ASSERT(info->args_len >= 1, "wrong arity: expected >=1, got %i", info->args_len);
  float maxval = -INFINITY;
  int maxval_i = INT_MIN;
  bool all_ints = true;
  for (int i = 0; i < info->args_len; i++) {
    float f;
    Value arg = load_arg(state->frame, INFO_ARGS_PTR(info)[i]);
    if (LIKELY(IS_FLOAT(arg))) { f = AS_FLOAT(arg); all_ints = false; }
    else if (IS_INT(arg)) {
      int k = AS_INT(arg);
      f = k;
      if (k > maxval_i) maxval_i = k;
    }
    else VM_ASSERT(false, "unexpected type for Math.max()");
    if (f > maxval) maxval = f;
  }
  if (all_ints) vm_return(state, info, INT2VAL(maxval_i));
  else vm_return(state, info, FLOAT2VAL(maxval));
}

static void min_fn(VMState *state, CallInfo *info) {
  VM_ASSERT(info->args_len >= 1, "wrong arity: expected >=1, got %i", info->args_len);
  float minval = INFINITY;
  int minval_i = INT_MAX;
  bool all_ints = true;
  for (int i = 0; i < info->args_len; i++) {
    float f;
    Value arg = load_arg(state->frame, INFO_ARGS_PTR(info)[i]);
    if (LIKELY(IS_FLOAT(arg))) { f = AS_FLOAT(arg); all_ints = false; }
    else if (IS_INT(arg)) {
      int k = AS_INT(arg);
      f = k;
      if (k < minval_i) minval_i = k;
    }
    else VM_ASSERT(false, "unexpected type for Math.min()");
    if (f < minval) minval = f;
  }
  if (all_ints) vm_return(state, info, INT2VAL(minval_i));
  else vm_return(state, info, FLOAT2VAL(minval));
}

static void rand_fn(VMState *state, CallInfo *info) {
  VM_ASSERT(info->args_len == 0, "wrong arity: expected 0, got %i", info->args_len);
  int res = rand();
  vm_return(state, info, INT2VAL(res));
}

static void randf_fn(VMState *state, CallInfo *info) {
  VM_ASSERT(info->args_len == 0, "wrong arity: expected 0, got %i", info->args_len);
  float res = rand() * 1.0f / RAND_MAX;
  vm_return(state, info, FLOAT2VAL(res));
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
  if (obj == state->shared->vcache.closure_base) return "function";
  if (obj == state->shared->vcache.function_base) return "sysfun";
  if (obj == state->shared->vcache.array_base) return "array";
  if (obj == state->shared->vcache.string_base) return "string";
  if (obj == state->shared->vcache.pointer_base) return "pointer";
  
  if (obj->parent) return get_type_info(state, OBJ2VAL(obj->parent));
  return "unknown";
}

static char *dir_sub(char *path_a, char *path_b) {
  if (path_a[strlen(path_a) - 1] == '/') return my_asprintf("%s%s", path_a, path_b);
  return my_asprintf("%s/%s", path_a, path_b);
}

static char *slice(char **str, char *needle) {
  char *pos = strstr(*str, needle);
  if (pos == NULL) {
    char *res = my_asprintf("%s", *str);
    *str = NULL;
    return res;
  }
  int offset = (int) (pos - *str);
  char *res = my_asprintf("%.*s", offset, *str);
  *str += offset + strlen(needle);
  return res;
}

void setup_default_searchpath(VMState *state, Object *root) {
  // search path: as per https://specifications.freedesktop.org/basedir-spec/basedir-spec-latest.html
  // current path | $XDG_DATA_HOME/jerboa | $XDG_DATA_DIRS/jerboa
  int paths_len = 0;
  Value *paths_ptr = NULL;
  
  char *cwd = get_current_dir_name();
  paths_ptr = realloc(paths_ptr, sizeof(Value) * ++paths_len);
  paths_ptr[paths_len - 1] = make_string_static(state, cwd);
  
#ifdef _WIN32
  TCHAR ownPath[MAX_PATH];
  DWORD len = GetModuleFileName(NULL, ownPath, MAX_PATH);
  if (len == MAX_PATH || len == 0) {
    fprintf(stderr, "cannot determine path of executable\n");
    abort();
  }
  ownPath[len] = 0;
  PathRemoveFileSpec(ownPath);
  
  paths_ptr = realloc(paths_ptr, sizeof(Value) * ++paths_len);
  paths_ptr[paths_len - 1] = make_string_static(state, my_asprintf("%s", ownPath));
#else  
  char *readlink_buf = malloc(1024);
  int rl_res = readlink("/proc/self/exe", readlink_buf, 1023);
  if (rl_res == -1) {
    fprintf(stderr, "cannot determine path of executable: %s\n", strerror(errno));
    abort();
  }
  readlink_buf[rl_res] = 0;
  
  char *share_dir = dir_sub(readlink_buf, "../share/jerboa");
  paths_ptr = realloc(paths_ptr, sizeof(Value) * ++paths_len);
  paths_ptr[paths_len - 1] = make_string_static(state, share_dir);
#endif
  
  char *xdg_home = getenv("XDG_DATA_HOME");
  if (xdg_home) xdg_home = dir_sub(xdg_home, "jerboa");
  else {
    xdg_home = getenv("HOME");
    if (xdg_home) {
      xdg_home = dir_sub(xdg_home, ".local/share/jerboa");
    }
  }
  if (xdg_home) {
    paths_ptr = realloc(paths_ptr, sizeof(Value) * ++paths_len);
    paths_ptr[paths_len - 1] = make_string_static(state, xdg_home);
  }
  
  char *xdg_dirs = getenv("XDG_DATA_DIRS");
  if (!xdg_dirs) xdg_dirs = "/usr/local/share/:/usr/share/";
  
  while (xdg_dirs) {
    char *xdg_dir = slice(&xdg_dirs, ":");
    assert(xdg_dir);
    char *xdg_jerboa_dir = dir_sub(xdg_dir, "jerboa");
    free(xdg_dir);
    
    paths_ptr = realloc(paths_ptr, sizeof(Value) * ++paths_len);
    paths_ptr[paths_len - 1] = make_string_static(state, xdg_jerboa_dir);
  }
  
  OBJECT_SET_STRING(state, root, "searchpath", make_array(state, paths_ptr, paths_len, true));
}

Object *create_root(VMState *state) {
  Object *root = AS_OBJ(make_object(state, NULL, false));
  
  state->root = root;
  
  GCRootSet pin_root;
  Value rootval = OBJ2VAL(root);
  gc_add_roots(state, &rootval, 1, &pin_root);
  
  Object *function_obj = AS_OBJ(make_object(state, NULL, false));
  function_obj->flags |= OBJ_NOINHERIT;
  OBJECT_SET_STRING(state, root, "sysfun", OBJ2VAL(function_obj));
  state->shared->vcache.function_base = function_obj;
  OBJECT_SET_STRING(state, function_obj, "apply", make_fn(state, fn_apply_fn));
  OBJECT_SET_STRING(state, function_obj, "call", make_fn(state, fn_call_fn));
  
  Object *int_obj = AS_OBJ(make_object(state, NULL, false));
  int_obj->flags |= OBJ_NOINHERIT;
  OBJECT_SET_STRING(state, root, "int", OBJ2VAL(int_obj));
  OBJECT_SET_STRING(state, int_obj, "+" , make_fn(state, int_add_fn));
  OBJECT_SET_STRING(state, int_obj, "-" , make_fn(state, int_sub_fn));
  OBJECT_SET_STRING(state, int_obj, "*" , make_fn(state, int_mul_fn));
  OBJECT_SET_STRING(state, int_obj, "/" , make_fn(state, int_div_fn));
  OBJECT_SET_STRING(state, int_obj, "%" , make_fn(state, int_mod_fn));
  OBJECT_SET_STRING(state, int_obj, "|" , make_fn(state, int_bit_or_fn));
  OBJECT_SET_STRING(state, int_obj, "&" , make_fn(state, int_bit_and_fn));
  OBJECT_SET_STRING(state, int_obj, "==", make_fn_fast(state, int_eq_fn, int_eq_fn_dispatch));
  OBJECT_SET_STRING(state, int_obj, "<" , make_fn(state, int_lt_fn));
  OBJECT_SET_STRING(state, int_obj, ">" , make_fn(state, int_gt_fn));
  OBJECT_SET_STRING(state, int_obj, "<=", make_fn(state, int_le_fn));
  OBJECT_SET_STRING(state, int_obj, ">=", make_fn(state, int_ge_fn));
  OBJECT_SET_STRING(state, int_obj, "parse" , make_fn(state, int_parse_fn));
  state->shared->vcache.int_base = int_obj;
  int_obj->flags |= OBJ_FROZEN;
  
  Object *float_obj = AS_OBJ(make_object(state, NULL, false));
  float_obj->flags |= OBJ_NOINHERIT;
  OBJECT_SET_STRING(state, root, "float", OBJ2VAL(float_obj));
  OBJECT_SET_STRING(state, float_obj, "+" , make_fn(state, float_add_fn));
  OBJECT_SET_STRING(state, float_obj, "-" , make_fn(state, float_sub_fn));
  OBJECT_SET_STRING(state, float_obj, "*" , make_fn(state, float_mul_fn));
  OBJECT_SET_STRING(state, float_obj, "/" , make_fn(state, float_div_fn));
  OBJECT_SET_STRING(state, float_obj, "%" , make_fn(state, float_mod_fn));
  OBJECT_SET_STRING(state, float_obj, "==", make_fn(state, float_eq_fn));
  OBJECT_SET_STRING(state, float_obj, "<" , make_fn(state, float_lt_fn));
  OBJECT_SET_STRING(state, float_obj, ">" , make_fn(state, float_gt_fn));
  OBJECT_SET_STRING(state, float_obj, "<=", make_fn(state, float_le_fn));
  OBJECT_SET_STRING(state, float_obj, ">=", make_fn(state, float_ge_fn));
  OBJECT_SET_STRING(state, float_obj, "toInt" , make_fn(state, float_toint_fn));
  state->shared->vcache.float_base = float_obj;
  float_obj->flags |= OBJ_FROZEN;
  
  Object *closure_obj = AS_OBJ(make_object(state, NULL, false));
  closure_obj->flags |= OBJ_NOINHERIT;
  OBJECT_SET_STRING(state, root, "function", OBJ2VAL(closure_obj));
  OBJECT_SET_STRING(state, closure_obj, "apply", make_fn(state, fn_apply_fn));
  OBJECT_SET_STRING(state, closure_obj, "call", make_fn(state, fn_call_fn));
  state->shared->vcache.closure_base = closure_obj;
  
  Object *bool_obj = AS_OBJ(make_object(state, NULL, false));
  bool_obj->flags |= OBJ_NOINHERIT;
  OBJECT_SET_STRING(state, root, "bool", OBJ2VAL(bool_obj));
  OBJECT_SET_STRING(state, bool_obj, "==", make_fn(state, bool_eq_fn));
  state->shared->vcache.bool_base = bool_obj;
  bool_obj->flags |= OBJ_FROZEN;
  
  OBJECT_SET_STRING(state, root, "true", BOOL2VAL(true));
  OBJECT_SET_STRING(state, root, "false", BOOL2VAL(false));
  
  OBJECT_SET_STRING(state, root, "null", VNULL);
  
  Object *string_obj = AS_OBJ(make_object(state, NULL, false));
  string_obj->flags |= OBJ_NOINHERIT;
  OBJECT_SET_STRING(state, root, "string", OBJ2VAL(string_obj));
  OBJECT_SET_STRING(state, string_obj, "+", make_fn(state, string_add_fn));
  OBJECT_SET_STRING(state, string_obj, "==", make_fn(state, string_eq_fn));
  OBJECT_SET_STRING(state, string_obj, "startsWith", make_fn(state, string_startswith_fn));
  OBJECT_SET_STRING(state, string_obj, "endsWith", make_fn(state, string_endswith_fn));
  OBJECT_SET_STRING(state, string_obj, "slice", make_fn(state, string_slice_fn));
  OBJECT_SET_STRING(state, string_obj, "find", make_fn(state, string_find_fn));
  OBJECT_SET_STRING(state, string_obj, "replace", make_fn(state, string_replace_fn));
  OBJECT_SET_STRING(state, string_obj, "byte_len", make_fn(state, string_byte_len_fn));
  state->shared->vcache.string_base = string_obj;
  
  Object *array_obj = AS_OBJ(make_object(state, NULL, false));
  array_obj->flags |= OBJ_NOINHERIT;
  OBJECT_SET_STRING(state, root, "array", OBJ2VAL(array_obj));
  OBJECT_SET_STRING(state, array_obj, "resize", make_fn(state, array_resize_fn));
  OBJECT_SET_STRING(state, array_obj, "push", make_fn(state, array_push_fn));
  OBJECT_SET_STRING(state, array_obj, "pop", make_fn(state, array_pop_fn));
  OBJECT_SET_STRING(state, array_obj, "[]", make_fn(state, array_index_fn));
  OBJECT_SET_STRING(state, array_obj, "in", make_fn(state, array_in_fn));
  OBJECT_SET_STRING(state, array_obj, "[]=", make_fn(state, array_index_assign_fn));
  OBJECT_SET_STRING(state, array_obj, "==", make_fn(state, array_compare_fn));
  OBJECT_SET_STRING(state, array_obj, "iterator", make_fn(state, array_iterator_fn));
  OBJECT_SET_STRING(state, array_obj, "splice", make_fn(state, array_splice_fn));
  OBJECT_SET_STRING(state, array_obj, "join", make_fn(state, array_join_fn));
  OBJECT_SET_STRING(state, array_obj, "dup", make_fn(state, array_dup_fn));
  state->shared->vcache.array_base = array_obj;
  
  Object *ptr_obj = AS_OBJ(make_object(state, NULL, false));
  ptr_obj->flags |= OBJ_NOINHERIT;
  OBJECT_SET_STRING(state, root, "pointer", OBJ2VAL(ptr_obj));
  OBJECT_SET_STRING(state, ptr_obj, "null", make_fn(state, ptr_is_null_fn));
  state->shared->vcache.pointer_base = ptr_obj;
  
  OBJECT_SET_STRING(state, root, "keys", make_fn(state, keys_fn));
  
  Object *xml_obj = AS_OBJ(make_object(state, NULL, false));
  OBJECT_SET_STRING(state, xml_obj, "load", make_fn(state, xml_load_fn));
  OBJECT_SET_STRING(state, xml_obj, "parse", make_fn(state, xml_parse_fn));
  // no! allow methods to redefine parse()
  // (it's not inner-loop enough to require freezing)
  // xml_obj->flags |= OBJ_FROZEN;
  
  Object *node_obj = AS_OBJ(make_object(state, NULL, false));
  OBJECT_SET_STRING(state, node_obj, "find_array", make_fn(state, xml_node_find_array_fn));
  OBJECT_SET_STRING(state, node_obj, "find_array_by_name", make_fn(state, xml_node_find_by_name_array_fn));
  OBJECT_SET_STRING(state, node_obj, "find_array_by_attr", make_fn(state, xml_node_find_by_attr_array_fn));
  OBJECT_SET_STRING(state, xml_obj, "node", OBJ2VAL(node_obj));
  
  Object *element_node_obj = AS_OBJ(make_object(state, node_obj, false));
  OBJECT_SET_STRING(state, element_node_obj, "nodeName", make_string_static(state, ""));
  OBJECT_SET_STRING(state, element_node_obj, "nodeType", INT2VAL(1));
  OBJECT_SET_STRING(state, element_node_obj, "attr", make_object(state, NULL, false));
  OBJECT_SET_STRING(state, element_node_obj, "children", make_array(state, NULL, 0, true));
  OBJECT_SET_STRING(state, element_node_obj, "parent", VNULL);
  OBJECT_SET_STRING(state, xml_obj, "element_node", OBJ2VAL(element_node_obj));
  
  Object *text_node_obj = AS_OBJ(make_object(state, node_obj, false));
  OBJECT_SET_STRING(state, text_node_obj, "nodeType", INT2VAL(3));
  OBJECT_SET_STRING(state, text_node_obj, "value", VNULL);
  OBJECT_SET_STRING(state, xml_obj, "text_node", OBJ2VAL(text_node_obj));
  
  OBJECT_SET_STRING(state, root, "xml", OBJ2VAL(xml_obj));
  
  Object *file_obj = AS_OBJ(make_object(state, NULL, false));
  OBJECT_SET_STRING(state, file_obj, "_handle", VNULL);
  OBJECT_SET_STRING(state, file_obj, "print", make_fn(state, file_print_fn));
  OBJECT_SET_STRING(state, file_obj, "exists", make_fn(state, file_exists_fn));
  OBJECT_SET_STRING(state, file_obj, "open", make_fn(state, file_open_fn));
  OBJECT_SET_STRING(state, file_obj, "close", make_fn(state, file_close_fn));
  file_obj->flags |= OBJ_FROZEN;
  OBJECT_SET_STRING(state, root, "file", OBJ2VAL(file_obj));
  
  Object *stdout_obj = AS_OBJ(make_object(state, file_obj, false));
  OBJECT_SET_STRING(state, stdout_obj, "_handle", make_ptr(state, (void*) stdout));
  OBJECT_SET_STRING(state, root, "stdout", OBJ2VAL(stdout_obj));
  
  Object *stderr_obj = AS_OBJ(make_object(state, file_obj, false));
  OBJECT_SET_STRING(state, stderr_obj, "_handle", make_ptr(state, (void*) stderr));
  OBJECT_SET_STRING(state, root, "stderr", OBJ2VAL(stderr_obj));
  
  OBJECT_SET_STRING(state, root, "print", make_fn(state, print_fn));
  
  setup_default_searchpath(state, root);
  
  OBJECT_SET_STRING(state, root, "require", make_fn(state, require_fn));
  OBJECT_SET_STRING(state, root, "_mark_const", make_fn(state, mark_const_fn));
  OBJECT_SET_STRING(state, root, "assert", make_fn(state, assert_fn));
  
  Object *math_obj = AS_OBJ(make_object(state, NULL, false));
  OBJECT_SET_STRING(state, math_obj, "sin", make_fn(state, sin_fn));
  OBJECT_SET_STRING(state, math_obj, "cos", make_fn(state, cos_fn));
  OBJECT_SET_STRING(state, math_obj, "tan", make_fn(state, tan_fn));
  OBJECT_SET_STRING(state, math_obj, "log", make_fn(state, log_fn));
  OBJECT_SET_STRING(state, math_obj, "sqrt", make_fn(state, sqrt_fn));
  OBJECT_SET_STRING(state, math_obj, "pow", make_fn(state, pow_fn));
  OBJECT_SET_STRING(state, math_obj, "max", make_fn(state, max_fn));
  OBJECT_SET_STRING(state, math_obj, "min", make_fn(state, min_fn));
  OBJECT_SET_STRING(state, math_obj, "rand", make_fn(state, rand_fn));
  OBJECT_SET_STRING(state, math_obj, "randf", make_fn(state, randf_fn));
  math_obj->flags |= OBJ_FROZEN;
  OBJECT_SET_STRING(state, root, "Math", OBJ2VAL(math_obj));
  
  Object *obj_tools = AS_OBJ(make_object(state, NULL, false));
  obj_tools->flags |= OBJ_NOINHERIT;
  OBJECT_SET_STRING(state, obj_tools, "keys", make_fn(state, obj_keys_fn));
  OBJECT_SET_STRING(state, obj_tools, "freeze", make_fn(state, freeze_fn));
  OBJECT_SET_STRING(state, obj_tools, "close", make_fn(state, close_fn));
  
  OBJECT_SET_STRING(state, root, "Object", OBJ2VAL(obj_tools));
  
  ffi_setup_root(state, root);
  
  root->flags |= OBJ_FROZEN;
  gc_remove_roots(state, &pin_root);
  
  return root;
}
