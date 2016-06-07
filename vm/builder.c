#include "vm/builder.h"

int new_block(FunctionBuilder *builder) {
  FunctionBody *body = &builder->body;
  body->blocks_len ++;
  body->blocks_ptr = realloc(body->blocks_ptr, body->blocks_len * sizeof(InstrBlock));
  body->blocks_ptr[body->blocks_len - 1] = (InstrBlock){NULL, 0};
  return body->blocks_len - 1;
}

static void addinstr(FunctionBuilder *builder, Instr *instr) {
  FunctionBody *body = &builder->body;
  InstrBlock *block = &body->blocks_ptr[body->blocks_len - 1];
  block->instrs_len ++;
  block->instrs_ptr = realloc(block->instrs_ptr, block->instrs_len * sizeof(Instr*));
  block->instrs_ptr[block->instrs_len - 1] = instr;
}

int addinstr_access(FunctionBuilder *builder, int obj_slot, int key_slot) {
  AccessInstr *instr = malloc(sizeof(AccessInstr));
  instr->base.type = INSTR_ACCESS;
  instr->target_slot = builder->slot_base++;
  instr->obj_slot = obj_slot;
  instr->key_slot = key_slot;
  addinstr(builder, (Instr*) instr);
  return instr->target_slot;
}

void addinstr_assign(FunctionBuilder *builder, int obj, int key_slot, int slot) {
  AssignInstr *instr = malloc(sizeof(AssignInstr));
  instr->base.type = INSTR_ASSIGN;
  instr->obj_slot = obj;
  instr->value_slot = slot;
  instr->key_slot = key_slot;
  addinstr(builder, (Instr*) instr);
}

void addinstr_assign_existing(FunctionBuilder *builder, int obj, int key_slot, int slot) {
  AssignExistingInstr *instr = malloc(sizeof(AssignExistingInstr));
  instr->base.type = INSTR_ASSIGN_EXISTING;
  instr->obj_slot = obj;
  instr->value_slot = slot;
  instr->key_slot = key_slot;
  addinstr(builder, (Instr*) instr);
}

void addinstr_close_object(FunctionBuilder *builder, int obj) {
  CloseObjectInstr *instr = malloc(sizeof(CloseObjectInstr));
  instr->base.type = INSTR_CLOSE_OBJECT;
  instr->slot = obj;
  addinstr(builder, (Instr*) instr);
}

int addinstr_get_context(FunctionBuilder *builder) {
  GetContextInstr *instr = malloc(sizeof(GetContextInstr));
  instr->base.type = INSTR_GET_CONTEXT;
  instr->slot = builder->slot_base++;
  addinstr(builder, (Instr*) instr);
  return instr->slot;
}

int addinstr_alloc_object(FunctionBuilder *builder, int parent) {
  AllocObjectInstr *instr = malloc(sizeof(AllocObjectInstr));
  instr->base.type = INSTR_ALLOC_OBJECT;
  instr->target_slot = builder->slot_base++;
  instr->parent_slot = parent;
  addinstr(builder, (Instr*) instr);
  return instr->target_slot;
}

int addinstr_alloc_int_object(FunctionBuilder *builder, int ctxslot, int value) {
  AllocIntObjectInstr *instr = malloc(sizeof(AllocIntObjectInstr));
  instr->base.type = INSTR_ALLOC_INT_OBJECT;
  instr->target_slot = builder->slot_base++;
  instr->value = value;
  addinstr(builder, (Instr*) instr);
  return instr->target_slot;
}

int addinstr_alloc_float_object(FunctionBuilder *builder, int ctxslot, float value) {
  AllocFloatObjectInstr *instr = malloc(sizeof(AllocFloatObjectInstr));
  instr->base.type = INSTR_ALLOC_FLOAT_OBJECT;
  instr->target_slot = builder->slot_base++;
  instr->value = value;
  addinstr(builder, (Instr*) instr);
  return instr->target_slot;
}

int addinstr_alloc_string_object(FunctionBuilder *builder, int ctxslot, char *value) {
  AllocStringObjectInstr *instr = malloc(sizeof(AllocStringObjectInstr));
  instr->base.type = INSTR_ALLOC_STRING_OBJECT;
  instr->target_slot = builder->slot_base++;
  instr->value = value;
  addinstr(builder, (Instr*) instr);
  return instr->target_slot;
}

int addinstr_alloc_closure_object(FunctionBuilder *builder, int ctxslot, UserFunction *fn) {
  AllocClosureObjectInstr *instr = malloc(sizeof(AllocClosureObjectInstr));
  instr->base.type = INSTR_ALLOC_CLOSURE_OBJECT;
  instr->target_slot = builder->slot_base++;
  instr->context_slot = ctxslot;
  instr->fn = fn;
  addinstr(builder, (Instr*) instr);
  return instr->target_slot;
}

int addinstr_call(FunctionBuilder *builder, int fn, int this_slot, int *args_ptr, int args_len) {
  CallInstr *instr = malloc(sizeof(CallInstr));
  instr->base.type = INSTR_CALL;
  instr->target_slot = builder->slot_base++;
  instr->function_slot = fn;
  instr->this_slot = this_slot;
  instr->args_length = args_len;
  instr->args_ptr = args_ptr;
  
  addinstr(builder, (Instr*) instr);
  return instr->target_slot;
}


int addinstr_call1(FunctionBuilder *builder, int fn, int this_slot, int arg0) {
  int *args = malloc(sizeof(int) * 1);
  args[0] = arg0;
  return addinstr_call(builder, fn, this_slot, args, 1);
}

int addinstr_call2(FunctionBuilder *builder, int fn, int this_slot, int arg0, int arg1) {
  int *args = malloc(sizeof(int) * 2);
  args[0] = arg0;
  args[1] = arg1;
  return addinstr_call(builder, fn, this_slot, args, 2);
}

void addinstr_test_branch(FunctionBuilder *builder, int test, int **truebranch, int **falsebranch) {
  TestBranchInstr *instr = malloc(sizeof(TestBranchInstr));
  instr->base.type = INSTR_TESTBR;
  instr->test_slot = test;
  *truebranch = &instr->true_blk;
  *falsebranch = &instr->false_blk;
  
  addinstr(builder, (Instr*) instr);
}

void addinstr_branch(FunctionBuilder *builder, int **branch) {
  BranchInstr *instr = malloc(sizeof(BranchInstr));
  instr->base.type = INSTR_BR;
  *branch = &instr->blk;
  
  addinstr(builder, (Instr*) instr);
}

void addinstr_return(FunctionBuilder *builder, int slot) {
  ReturnInstr *instr = malloc(sizeof(ReturnInstr));
  instr->base.type = INSTR_RETURN;
  instr->ret_slot = slot;
  
  addinstr(builder, (Instr*) instr);
}

UserFunction *build_function(FunctionBuilder *builder) {
  UserFunction *fn = malloc(sizeof(UserFunction));
  fn->arity = builder->arglist_len;
  fn->slots = builder->slot_base;
  fn->name = builder->name;
  fn->body = builder->body;
  return fn;
}
