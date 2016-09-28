#ifndef VALUE_KIND
#define VALUE_KIND instr->value.kind
#define VALUE_KIND_DEFINED
#endif

#ifndef TARGET_KIND
#define TARGET_KIND instr->target.kind
#define TARGET_KIND_DEFINED
#endif

#ifndef FN_NAME
#define FN_NAME vm_instr_test
#define FN_NAME_DEFINED
#endif

#include "core.h"
#include "object.h"

static FnWrap FN_NAME(VMState * __restrict__ state) FAST_FN;
static FnWrap FN_NAME(VMState * __restrict__ state) {
  TestInstr * __restrict__ instr = (TestInstr*) state->instr;
  Callframe * __restrict__ frame = state->frame;
  Value val = load_arg_specialized(frame, instr->value, VALUE_KIND);
  bool res = value_is_truthy(val);
  set_arg_specialized(state, instr->target, BOOL2VAL(res), TARGET_KIND);
  state->instr = (Instr*) (instr + 1);
  return (FnWrap) { state->instr->fn };
}

#ifdef VALUE_KIND_DEFINED
#undef VALUE_KIND_DEFINED
#undef VALUE_KIND
#endif

#ifdef TARGET_KIND_DEFINED
#undef TARGET_KIND_DEFINED
#undef TARGET_KIND
#endif

#ifdef FN_NAME_DEFINED
#undef FN_NAME_DEFINED
#undef FN_NAME
#endif
