#ifndef TEST_KIND
#define TEST_KIND instr->test.kind
#define TEST_KIND_DEFINED
#endif

#ifndef FN_NAME
#define FN_NAME vm_instr_testbr
#define FN_NAME_DEFINED
#endif

#include "core.h"
#include "object.h"

static FnWrap FN_NAME(VMState * __restrict__ state) FAST_FN;
static FnWrap FN_NAME(VMState * __restrict__ state) {
  TestBranchInstr * __restrict__ instr = (TestBranchInstr*) state->instr;
  Callframe * __restrict__ frame = state->frame;
  int true_blk = instr->true_blk, false_blk = instr->false_blk;
  assert(instr->test.kind != ARG_REFSLOT); // TODO document why
  Value test_value = load_arg_specialized(frame, instr->test, TEST_KIND);
  VM_ASSERT2_DEBUG(IS_BOOL(test_value), "can't branch on non-bool");
  bool test = AS_BOOL(test_value);
  
  int target_blk = test ? true_blk : false_blk;
  state->instr = (Instr*) ((char*) frame->uf->body.instrs_ptr + frame->uf->body.blocks_ptr[target_blk].offset);
  frame->prev_block = frame->block;
  frame->block = target_blk;
  STEP_VM;
}

#ifdef TEST_KIND_DEFINED
#undef TEST_KIND_DEFINED
#undef TEST_KIND
#endif

#ifdef FN_NAME_DEFINED
#undef FN_NAME_DEFINED
#undef FN_NAME
#endif
