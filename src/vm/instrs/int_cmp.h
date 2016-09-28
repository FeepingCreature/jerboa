#ifndef LHS_KIND
#define LHS_KIND info->this_arg.kind
#define LHS_KIND_DEFINED
#endif

#ifndef RHS_KIND
#define RHS_KIND INFO_ARGS_PTR(info)[0].kind
#define RHS_KIND_DEFINED
#endif

#ifndef TARGET_KIND
#define TARGET_KIND info->target.kind
#define TARGET_KIND_DEFINED
#endif

#ifndef FN_NAME
#define FN_NAME int_cmp_fn
#define FN_NAME_DEFINED
#endif

#include "core.h"
#include "object.h"
#include "vm/runtime.h"

static inline void FN_NAME(VMState *state, CallInfo *info, CompareOp cmp) __attribute__ ((always_inline));
static inline void FN_NAME(VMState *state, CallInfo *info, CompareOp cmp) {
  VM_ASSERT(info->args_len == 1, "wrong arity: expected 1, got %i", info->args_len);
  
  Value val1 = load_arg_specialized(state->frame, info->this_arg, LHS_KIND);
  Value val2 = load_arg_specialized(state->frame, INFO_ARGS_PTR(info)[0], RHS_KIND);
  
  VM_ASSERT(IS_INT(val1), "internal error: int compare function called on wrong type of object");
  if (LIKELY(IS_INT(val2))) { // int cmp mostly with int
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
    vm_return_specialized(state, info, BOOL2VAL(res), TARGET_KIND);
    return;
  }
  
  if (LIKELY(IS_FLOAT(val2))) { // okay, so almost definitely float
    int i1 = AS_INT(val1), v2 = AS_FLOAT(val2);
    bool res;
    switch (cmp) {
      case CMP_EQ: res = i1 == v2; break;
      case CMP_LT: res = i1 <  v2; break;
      case CMP_GT: res = i1 >  v2; break;
      case CMP_LE: res = i1 <= v2; break;
      case CMP_GE: res = i1 >= v2; break;
      default: abort();
    }
    vm_return_specialized(state, info, BOOL2VAL(res), TARGET_KIND);
    return;
  }
  VM_ASSERT(false, "don't know how to compare int with %s", get_type_info(state, load_arg(state->frame, INFO_ARGS_PTR(info)[0])));
}

#ifdef LHS_KIND_DEFINED
#undef LHS_KIND_DEFINED
#undef LHS_KIND
#endif

#ifdef RHS_KIND_DEFINED
#undef RHS_KIND_DEFINED
#undef RHS_KIND
#endif

#ifdef TARGET_KIND_DEFINED
#undef TARGET_KIND_DEFINED
#undef TARGET_KIND
#endif

#ifdef FN_NAME_DEFINED
#undef FN_NAME_DEFINED
#undef FN_NAME
#endif
