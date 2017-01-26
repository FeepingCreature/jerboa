#ifndef VALUE_KIND
#define VALUE_KIND instr->ret.kind
#define VALUE_KIND_DEFINED
#endif

#ifndef FN_NAME
#define FN_NAME vm_instr_return
#define FN_NAME_DEFINED
#endif

#include "core.h"
#include "object.h"
#include "gc.h"

static FnWrap FN_NAME(VMState * __restrict__ state) FAST_FN;
static FnWrap FN_NAME(VMState * __restrict__ state) {
  ReturnInstr * __restrict__ instr = (ReturnInstr*) state->instr;
  assert(instr->ret.kind != ARG_REFSLOT);
  Value res = load_arg_specialized(state->frame, instr->ret, VALUE_KIND);
  WriteArg target = state->frame->target;
  gc_remove_roots(state, &state->frame->frameroot_slots);
  vm_remove_frame(state);
  
  set_arg(state, target, res);
  
  if (UNLIKELY(!state->frame)) {
    return (FnWrap) { vm_halt };
  }
  
#ifndef NDEBUG
  state->instr = state->frame->instr_ptr;
  state->instr = (Instr*)((char*) state->instr + instr_size(state->instr));
  assert(state->instr == state->frame->return_next_instr);
#else
  state->instr = state->frame->return_next_instr;
#endif
  
  STEP_VM;
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
