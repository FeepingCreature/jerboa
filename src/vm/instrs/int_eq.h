#ifndef FN_NAME
#define FN_NAME int_cmp_fn
#define FN_NAME_DEFINED
#endif

#ifndef FAST_FN_NAME
#define FAST_FN_NAME int_eq_fn_fast
#define FAST_FN_NAME_DEFINED
#endif

#include "core.h"
#include "object.h"
#include "vm/runtime.h"

static FnWrap FAST_FN_NAME(VMState * __restrict__ state) FAST_FN;
static FnWrap FAST_FN_NAME(VMState * __restrict__ state) {
  CallFunctionDirectInstr * __restrict__ instr = (CallFunctionDirectInstr*) state->instr;
  CallInfo * __restrict__ info = &instr->info;
  
  Instr *next_instr = (Instr*) ((char*) instr + instr->size);
  FN_NAME(state, info, CMP_EQ);
  if (UNLIKELY(state->runstate != VM_RUNNING)) return (FnWrap) { vm_halt };
  state->instr = next_instr;
  STEP_VM;
}

#ifdef FN_NAME_DEFINED
#undef FN_NAME_DEFINED
#undef FN_NAME
#endif

#ifdef FAST_FN_NAME_DEFINED
#undef FAST_FN_NAME_DEFINED
#undef FAST_FN_NAME
#endif
