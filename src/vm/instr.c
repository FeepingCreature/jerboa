#include "vm/instr.h"

#include <stdio.h>
#include <stdlib.h>

#include "object.h"

char *get_arg_info(Arg arg) {
  if (arg.kind == ARG_SLOT) return my_asprintf("%%%i", arg.slot);
  if (arg.kind == ARG_REFSLOT) return my_asprintf("&%i", arg.refslot);
  assert(arg.kind == ARG_VALUE);
  return get_val_info(arg.value);
}

char *get_arg_info_ext(VMState *state, Arg arg) {
  if (arg.kind == ARG_SLOT) return my_asprintf("%%%i", arg.slot);
  if (arg.kind == ARG_REFSLOT) return my_asprintf("&%i", arg.refslot);
  assert(arg.kind == ARG_VALUE);
  return get_val_info_ext(state, arg.value);
}

char *get_write_arg_info(WriteArg warg) {
  if (warg.kind == ARG_SLOT) return my_asprintf("%%%i", warg.slot);
  if (warg.kind == ARG_REFSLOT) return my_asprintf("&%i", warg.refslot);
  abort();
}

int instr_size(Instr *instr) {
  switch (instr->type) {
#define CASE(EN, TY) case EN: return sizeof(TY)
    CASE(INSTR_GET_ROOT, GetRootInstr);
    CASE(INSTR_ALLOC_OBJECT, AllocObjectInstr);
    CASE(INSTR_ALLOC_INT_OBJECT, AllocIntObjectInstr);
    CASE(INSTR_ALLOC_BOOL_OBJECT, AllocBoolObjectInstr);
    CASE(INSTR_ALLOC_FLOAT_OBJECT, AllocFloatObjectInstr);
    CASE(INSTR_ALLOC_ARRAY_OBJECT, AllocArrayObjectInstr);
    CASE(INSTR_ALLOC_STRING_OBJECT, AllocStringObjectInstr);
    CASE(INSTR_ALLOC_CLOSURE_OBJECT, AllocClosureObjectInstr);
    CASE(INSTR_CLOSE_OBJECT, CloseObjectInstr);
    CASE(INSTR_FREEZE_OBJECT, FreezeObjectInstr);
    CASE(INSTR_ACCESS, AccessInstr);
    CASE(INSTR_ASSIGN, AssignInstr);
    CASE(INSTR_KEY_IN_OBJ, KeyInObjInstr);
    CASE(INSTR_IDENTICAL, IdenticalInstr);
    CASE(INSTR_INSTANCEOF, InstanceofInstr);
    CASE(INSTR_SET_CONSTRAINT, SetConstraintInstr);
    CASE(INSTR_TEST, TestInstr);
    CASE(INSTR_RETURN, ReturnInstr);
    CASE(INSTR_BR, BranchInstr);
    CASE(INSTR_TESTBR, TestBranchInstr);
    CASE(INSTR_PHI, PhiInstr);
    CASE(INSTR_ACCESS_STRING_KEY, AccessStringKeyInstr);
    CASE(INSTR_ASSIGN_STRING_KEY, AssignStringKeyInstr);
    CASE(INSTR_STRING_KEY_IN_OBJ, StringKeyInObjInstr);
    CASE(INSTR_SET_CONSTRAINT_STRING_KEY, SetConstraintStringKeyInstr);
    CASE(INSTR_DEFINE_REFSLOT, DefineRefslotInstr);
    CASE(INSTR_MOVE, MoveInstr);
#undef CASE
    case INSTR_ALLOC_STATIC_OBJECT:
      return sizeof(AllocStaticObjectInstr)
      + sizeof(Object)
      + sizeof(StaticFieldInfo) * ((AllocStaticObjectInstr*)instr)->info_len;
    case INSTR_CALL: return ((CallInstr*)instr)->size;
    case INSTR_CALL_FUNCTION_DIRECT: return ((CallFunctionDirectInstr*)instr)->size;
    default: fprintf(stderr, "unknown instruction size for %i\n", instr->type); abort();
  }
}
