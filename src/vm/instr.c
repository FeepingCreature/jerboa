#include "vm/instr.h"

#include <stdlib.h>
#include <assert.h>

int instr_size(Instr *instr) {
  switch (instr->type) {
#define CASE(EN, TY) case EN: return sizeof(TY)
    CASE(INSTR_GET_ROOT, GetRootInstr); CASE(INSTR_GET_CONTEXT, GetContextInstr);
    CASE(INSTR_ALLOC_OBJECT, AllocObjectInstr);
    CASE(INSTR_ALLOC_INT_OBJECT, AllocIntObjectInstr);
    CASE(INSTR_ALLOC_FLOAT_OBJECT, AllocFloatObjectInstr);
    CASE(INSTR_ALLOC_ARRAY_OBJECT, AllocArrayObjectInstr);
    CASE(INSTR_ALLOC_STRING_OBJECT, AllocStringObjectInstr);
    CASE(INSTR_ALLOC_CLOSURE_OBJECT, AllocClosureObjectInstr);
    CASE(INSTR_CLOSE_OBJECT, CloseObjectInstr);
    CASE(INSTR_FREEZE_OBJECT, FreezeObjectInstr);
    CASE(INSTR_ACCESS, AccessInstr); CASE(INSTR_ASSIGN, AssignInstr);
    CASE(INSTR_CALL, CallInstr); CASE(INSTR_RETURN, ReturnInstr);
    CASE(INSTR_SAVE_RESULT, SaveResultInstr);
    CASE(INSTR_BR, BranchInstr); CASE(INSTR_TESTBR, TestBranchInstr);
    CASE(INSTR_ACCESS_STRING_KEY, AccessStringKeyInstr);
    CASE(INSTR_ASSIGN_STRING_KEY, AssignStringKeyInstr);
    CASE(INSTR_DEFINE_REFSLOT, DefineRefslotInstr);
    CASE(INSTR_READ_REFSLOT, ReadRefslotInstr);
    CASE(INSTR_WRITE_REFSLOT, WriteRefslotInstr);
#undef CASE
    default: abort();
  }
}
