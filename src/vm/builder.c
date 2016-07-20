#include "vm/builder.h"

#include "util.h"
#include "parser.h"

void record_start(char *text, FileRange *range) {
  TextRange line;
  eat_filler(&text); // record at the start of the [whatever], not on the end of the previous
  range->text_from = text;
  bool found = find_text_pos(text, (const char**) &range->file, &line, &range->row_from, &range->col_from);
  (void) found; assert(found);
}

void record_end(char *text, FileRange *range) {
  TextRange line;
  const char *file;
  range->text_to = text;
  bool found = find_text_pos(text, &file, &line, &range->row_to, &range->col_to);
  (void) found; assert(found);
  assert(strcmp(file, range->file) == 0);
}

void use_range_start(FunctionBuilder *builder, FileRange *range) {
  if (!builder) return;
  assert(builder->current_range == NULL);
  builder->current_range = range;
}

void use_range_end(FunctionBuilder *builder, FileRange *range) {
  if (!builder) return;
  (void) range; assert(builder->current_range == range);
  builder->current_range = NULL;
}

FileRange *alloc_and_record_start(char *text) {
  FileRange *range = calloc(sizeof(FileRange), 1);
  record_start(text, range);
  return range;
}

int new_block(FunctionBuilder *builder) {
  assert(builder->block_terminated);
  FunctionBody *body = &builder->body;
  int offset = (char*) body->instrs_ptr_end - (char*) body->instrs_ptr;
  body->blocks_len ++;
  body->blocks_ptr = realloc(body->blocks_ptr, body->blocks_len * sizeof(InstrBlock));
  body->blocks_ptr[body->blocks_len - 1] = (InstrBlock){offset, 0};
  builder->block_terminated = false;
  return body->blocks_len - 1;
}

void terminate(FunctionBuilder *builder) {
  // terminate with "return null"
  addinstr_return(builder, 0);
}

void addinstr(FunctionBuilder *builder, int size, Instr *instr) {
  assert(!builder->block_terminated);
  if (!builder->current_range) abort();
  instr->belongs_to = builder->current_range;
  instr->context_slot = builder->scope;
  FunctionBody *body = &builder->body;
  InstrBlock *block = &body->blocks_ptr[body->blocks_len - 1];
  int current_len = (char*) body->instrs_ptr_end - (char*) body->instrs_ptr;
  int new_len = current_len + size;
  body->instrs_ptr = realloc(body->instrs_ptr, new_len);
  body->instrs_ptr_end = (Instr*) ((char*) body->instrs_ptr + new_len);
  block->size += size;
  memcpy((char*) body->instrs_ptr + current_len, instr, size);
  if (instr->type == INSTR_BR || instr->type == INSTR_TESTBR || instr->type == INSTR_RETURN) {
    builder->block_terminated = true;
  }
}

void addinstr_like(FunctionBuilder *builder, Instr *basis, int size, Instr *instr) {
  int backup = builder->scope;
  use_range_start(builder, basis->belongs_to);
  builder->scope = basis->context_slot;
  addinstr(builder, size, instr);
  use_range_end(builder, basis->belongs_to);
  builder->scope = backup;
}

static int offset_to_instr_about_to_be_added(FunctionBuilder *builder, char *instr, char *ptr) {
  FunctionBody *body = &builder->body;
  int current_len = (char*) body->instrs_ptr_end - (char*) body->instrs_ptr;
  int delta = ptr - instr;
  return current_len + delta;
}

void set_int_var(FunctionBuilder *builder, int offset, int value) {
  FunctionBody *body = &builder->body;
  *(int*) ((char*) body->instrs_ptr + offset) = value;
}

int addinstr_access(FunctionBuilder *builder, int obj_slot, int key_slot) {
  AccessInstr *instr = malloc(sizeof(AccessInstr));
  instr->base.type = INSTR_ACCESS;
  instr->base.belongs_to = NULL;
  instr->obj_slot = obj_slot;
  instr->key_slot = key_slot;
  instr->target_slot = builder->slot_base++;
  addinstr(builder, sizeof(AccessInstr), (Instr*) instr);
  return instr->target_slot;
}

void addinstr_assign(FunctionBuilder *builder, int obj, int key_slot, int slot, AssignType type) {
  AssignInstr *instr = malloc(sizeof(AssignInstr));
  instr->base.type = INSTR_ASSIGN;
  instr->base.belongs_to = NULL;
  instr->obj_slot = obj;
  instr->value_slot = slot;
  instr->key_slot = key_slot;
  instr->type = type;
  addinstr(builder, sizeof(*instr), (Instr*) instr);
}

int addinstr_key_in_obj(FunctionBuilder *builder, int key_slot, int obj_slot) {
  KeyInObjInstr *instr = malloc(sizeof(KeyInObjInstr));
  instr->base.type = INSTR_KEY_IN_OBJ;
  instr->base.belongs_to = NULL;
  instr->key_slot = key_slot;
  instr->obj_slot = obj_slot;
  instr->target_slot = builder->slot_base++;
  addinstr(builder, sizeof(KeyInObjInstr), (Instr*) instr);
  return instr->target_slot;
}

void addinstr_set_constraint(FunctionBuilder *builder, int obj_slot, int key_slot, int cons_slot) {
  SetConstraintInstr instr;
  instr.base.type = INSTR_SET_CONSTRAINT;
  instr.base.belongs_to = NULL;
  instr.obj_slot = obj_slot;
  instr.key_slot = key_slot;
  instr.constraint_slot = cons_slot;
  addinstr(builder, sizeof(instr), (Instr*) &instr);
}

void addinstr_close_object(FunctionBuilder *builder, int obj) {
  CloseObjectInstr *instr = malloc(sizeof(CloseObjectInstr));
  instr->base.type = INSTR_CLOSE_OBJECT;
  instr->base.belongs_to = NULL;
  instr->slot = obj;
  addinstr(builder, sizeof(*instr), (Instr*) instr);
}

void addinstr_freeze_object(FunctionBuilder *builder, int obj) {
  FreezeObjectInstr *instr = malloc(sizeof(FreezeObjectInstr));
  instr->base.type = INSTR_FREEZE_OBJECT;
  instr->base.belongs_to = NULL;
  instr->slot = obj;
  addinstr(builder, sizeof(*instr), (Instr*) instr);
}

int addinstr_alloc_object(FunctionBuilder *builder, int parent) {
  AllocObjectInstr *instr = malloc(sizeof(AllocObjectInstr));
  instr->base.type = INSTR_ALLOC_OBJECT;
  instr->base.belongs_to = NULL;
  instr->target_slot = builder->slot_base++;
  instr->parent_slot = parent;
  addinstr(builder, sizeof(*instr), (Instr*) instr);
  return instr->target_slot;
}

int addinstr_alloc_int_object(FunctionBuilder *builder, int value) {
  AllocIntObjectInstr *instr = malloc(sizeof(AllocIntObjectInstr));
  instr->base.type = INSTR_ALLOC_INT_OBJECT;
  instr->base.belongs_to = NULL;
  instr->target_slot = builder->slot_base++;
  instr->value = value;
  addinstr(builder, sizeof(*instr), (Instr*) instr);
  return instr->target_slot;
}

int addinstr_alloc_float_object(FunctionBuilder *builder, float value) {
  AllocFloatObjectInstr *instr = malloc(sizeof(AllocFloatObjectInstr));
  instr->base.type = INSTR_ALLOC_FLOAT_OBJECT;
  instr->base.belongs_to = NULL;
  instr->target_slot = builder->slot_base++;
  instr->value = value;
  addinstr(builder, sizeof(*instr), (Instr*) instr);
  return instr->target_slot;
}

int addinstr_alloc_array_object(FunctionBuilder *builder) {
  AllocArrayObjectInstr *instr = malloc(sizeof(AllocArrayObjectInstr));
  instr->base.type = INSTR_ALLOC_ARRAY_OBJECT;
  instr->base.belongs_to = NULL;
  instr->target_slot = builder->slot_base++;
  addinstr(builder, sizeof(*instr), (Instr*) instr);
  return instr->target_slot;
}

int addinstr_alloc_string_object(FunctionBuilder *builder, char *value) {
  AllocStringObjectInstr *instr = malloc(sizeof(AllocStringObjectInstr));
  instr->base.type = INSTR_ALLOC_STRING_OBJECT;
  instr->base.belongs_to = NULL;
  instr->target_slot = builder->slot_base++;
  instr->value = value;
  addinstr(builder, sizeof(*instr), (Instr*) instr);
  return instr->target_slot;
}

int addinstr_alloc_closure_object(FunctionBuilder *builder, UserFunction *fn) {
  AllocClosureObjectInstr *instr = malloc(sizeof(AllocClosureObjectInstr));
  instr->base.type = INSTR_ALLOC_CLOSURE_OBJECT;
  instr->base.belongs_to = NULL;
  instr->target_slot = builder->slot_base++;
  instr->fn = fn;
  addinstr(builder, sizeof(*instr), (Instr*) instr);
  return instr->target_slot;
}

int addinstr_call(FunctionBuilder *builder, int fn, int this_slot, int *args_ptr, int args_len) {
  CallInstr *instr1 = malloc(sizeof(CallInstr));
  instr1->base.type = INSTR_CALL;
  instr1->base.belongs_to = NULL;
  instr1->function_slot = fn;
  instr1->this_slot = this_slot;
  instr1->args_length = args_len;
  instr1->args_ptr = args_ptr;
  addinstr(builder, sizeof(*instr1), (Instr*) instr1);
  
  SaveResultInstr *instr2 = malloc(sizeof(SaveResultInstr));
  instr2->base.type = INSTR_SAVE_RESULT;
  instr2->base.belongs_to = NULL;
  instr2->target_slot = builder->slot_base++;
  addinstr(builder, sizeof(*instr2), (Instr*) instr2);
  return instr2->target_slot;
}

int addinstr_call0(FunctionBuilder *builder, int fn, int this_slot) {
  return addinstr_call(builder, fn, this_slot, NULL, 0);
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

void addinstr_test_branch(FunctionBuilder *builder, int test, int *truebranch, int *falsebranch) {
  TestBranchInstr *instr = malloc(sizeof(TestBranchInstr));
  instr->base.type = INSTR_TESTBR;
  instr->base.belongs_to = NULL;
  instr->test_slot = test;
  *truebranch = offset_to_instr_about_to_be_added(builder, (char*) instr, (char*) &instr->true_blk);
  *falsebranch = offset_to_instr_about_to_be_added(builder, (char*) instr, (char*) &instr->false_blk);
  
  addinstr(builder, sizeof(*instr), (Instr*) instr);
}

void addinstr_branch(FunctionBuilder *builder, int *branch) {
  BranchInstr *instr = malloc(sizeof(BranchInstr));
  instr->base.type = INSTR_BR;
  instr->base.belongs_to = NULL;
  *branch = offset_to_instr_about_to_be_added(builder, (char*) instr, (char*) &instr->blk);
  
  addinstr(builder, sizeof(*instr), (Instr*) instr);
}

void addinstr_return(FunctionBuilder *builder, int slot) {
  ReturnInstr *instr = malloc(sizeof(ReturnInstr));
  instr->base.type = INSTR_RETURN;
  instr->base.belongs_to = NULL;
  instr->ret_slot = slot;
  
  addinstr(builder, sizeof(*instr), (Instr*) instr);
}

int addinstr_def_refslot(FunctionBuilder *builder, int obj_slot, char *key) {
  DefineRefslotInstr *instr = malloc(sizeof(DefineRefslotInstr));
  instr->base.type = INSTR_DEFINE_REFSLOT;
  instr->base.belongs_to = NULL;
  instr->obj_slot = obj_slot;
  instr->key_ptr = key;
  instr->key_len = strlen(key);
  instr->key_hash = hash(instr->key_ptr, instr->key_len);
  instr->target_refslot = builder->refslot_base ++;
  
  addinstr(builder, sizeof(*instr), (Instr*) instr);
  return instr->target_refslot;
}

void addinstr_read_refslot(FunctionBuilder *builder, int source_refslot, int target_slot, char *opt_info) {
  ReadRefslotInstr *instr = malloc(sizeof(ReadRefslotInstr));
  instr->base.type = INSTR_READ_REFSLOT;
  instr->base.belongs_to = NULL;
  instr->source_refslot = source_refslot;
  instr->target_slot = target_slot;
  instr->opt_info = opt_info;
  
  addinstr(builder, sizeof(*instr), (Instr*) instr);
}

void addinstr_write_refslot(FunctionBuilder *builder, int source_slot, int target_refslot, char *opt_info) {
  WriteRefslotInstr *instr = malloc(sizeof(WriteRefslotInstr));
  instr->base.type = INSTR_WRITE_REFSLOT;
  instr->base.belongs_to = NULL;
  instr->source_slot = source_slot;
  instr->target_refslot = target_refslot;
  instr->opt_info = opt_info;
  
  addinstr(builder, sizeof(*instr), (Instr*) instr);
}

UserFunction *build_function(FunctionBuilder *builder) {
  assert(builder->block_terminated);
  UserFunction *fn = malloc(sizeof(UserFunction));
  fn->arity = builder->arglist_len;
  fn->variadic_tail = builder->variadic_tail;
  fn->slots = builder->slot_base;
  fn->refslots = builder->refslot_base;
  fn->name = builder->name;
  fn->body = builder->body;
  fn->is_method = false;
  return fn;
}
