#include "vm/builder.h"

#include "core.h"
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

int get_block(FunctionBuilder *builder) {
  return builder->body.blocks_len - 1;
}

void terminate(FunctionBuilder *builder) {
  // terminate with "return null"
  addinstr_return(builder, 0);
}

void addinstr(FunctionBuilder *builder, int size, Instr *instr) {
  assert(!builder->block_terminated);
  if (!builder->current_range) abort();
  FunctionBody *body = &builder->body;
  InstrBlock *block = &body->blocks_ptr[body->blocks_len - 1];
  int current_len = (char*) body->instrs_ptr_end - (char*) body->instrs_ptr;
  int new_len = current_len + size;
  body->instrs_ptr = realloc(body->instrs_ptr, new_len);
  body->instrs_ptr_end = (Instr*) ((char*) body->instrs_ptr + new_len);
  block->size += size;
  Instr *new_instr = (Instr*) ((char*) body->instrs_ptr + current_len);
  memcpy((void*) new_instr, instr, size);
  new_instr->belongs_to = builder->current_range;
  new_instr->context_slot = builder->scope;
  if (instr->type == INSTR_BR || instr->type == INSTR_TESTBR || instr->type == INSTR_RETURN) {
    builder->block_terminated = true;
  }
}

#include <stdio.h>
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

int addinstr_get_root(FunctionBuilder *builder) {
  GetRootInstr instr = {
    .base = { .type = INSTR_GET_ROOT },
    .slot = builder->slot_base++
  };
  addinstr(builder, sizeof(instr), (Instr*) &instr);
  return instr.slot;
}

int addinstr_access(FunctionBuilder *builder, int obj_slot, int key_slot) {
  AccessInstr instr = {
    .base = { .type = INSTR_ACCESS },
    .obj = (Arg) { .kind = ARG_SLOT, .slot = obj_slot },
    .key = (Arg) { .kind = ARG_SLOT, .slot = key_slot },
    .target = (WriteArg) { .kind = ARG_SLOT, .slot = builder->slot_base++ }
  };
  addinstr(builder, sizeof(instr), (Instr*) &instr);
  return instr.target.slot;
}

void addinstr_assign(FunctionBuilder *builder, int obj, int key_slot, int slot, AssignType type) {
  AssignInstr instr = {
    .base = { .type = INSTR_ASSIGN },
    .obj = (Arg) { .kind = ARG_SLOT, .slot = obj },
    .value = (Arg) { .kind = ARG_SLOT, .slot = slot },
    .key = (Arg) { .kind = ARG_SLOT, .slot = key_slot },
    .target_slot = builder->slot_base++,
    .type = type
  };
  addinstr(builder, sizeof(instr), (Instr*) &instr);
}

int addinstr_key_in_obj(FunctionBuilder *builder, int key_slot, int obj_slot) {
  KeyInObjInstr instr = {
    .base = { .type = INSTR_KEY_IN_OBJ },
    .obj = (Arg) { .kind = ARG_SLOT, .slot = obj_slot },
    .key = (Arg) { .kind = ARG_SLOT, .slot = key_slot },
    .target = (WriteArg) { .kind = ARG_SLOT, .slot = builder->slot_base++ }
  };
  addinstr(builder, sizeof(instr), (Instr*) &instr);
  return instr.target.slot;
}

int addinstr_identical(FunctionBuilder *builder, int slot1, int slot2) {
  IdenticalInstr instr = {
    .base = { .type = INSTR_IDENTICAL },
    .obj1 = (Arg) { .kind = ARG_SLOT, .slot = slot1 },
    .obj2 = (Arg) { .kind = ARG_SLOT, .slot = slot2 },
    .target = (WriteArg) { .kind = ARG_SLOT, .slot = builder->slot_base++ }
  };
  addinstr(builder, sizeof(instr), (Instr*) &instr);
  return instr.target.slot;
}

int addinstr_instanceof(FunctionBuilder *builder, int obj_slot, int proto_slot) {
  InstanceofInstr instr = {
    .base = { .type = INSTR_INSTANCEOF },
    .obj = (Arg) { .kind = ARG_SLOT, .slot = obj_slot },
    .proto = (Arg) { .kind = ARG_SLOT, .slot = proto_slot },
    .target = (WriteArg) { .kind = ARG_SLOT, .slot = builder->slot_base++ }
  };
  addinstr(builder, sizeof(instr), (Instr*) &instr);
  return instr.target.slot;
}

void addinstr_set_constraint(FunctionBuilder *builder, int obj_slot, int key_slot, int cons_slot) {
  SetConstraintInstr instr = {
    .base = { .type = INSTR_SET_CONSTRAINT },
    .obj = (Arg) { .kind = ARG_SLOT, .slot = obj_slot },
    .key = (Arg) { .kind = ARG_SLOT, .slot = key_slot },
    .constraint = (Arg) { .kind = ARG_SLOT, .slot = cons_slot }
  };
  addinstr(builder, sizeof(instr), (Instr*) &instr);
}

void addinstr_close_object(FunctionBuilder *builder, int obj) {
  CloseObjectInstr instr = {
    .base = { .type = INSTR_CLOSE_OBJECT },
    .slot = obj
  };
  addinstr(builder, sizeof(instr), (Instr*) &instr);
}

void addinstr_freeze_object(FunctionBuilder *builder, int obj) {
  FreezeObjectInstr instr = {
    .base = { .type = INSTR_FREEZE_OBJECT },
    .slot = obj
  };
  addinstr(builder, sizeof(instr), (Instr*) &instr);
}

int addinstr_alloc_object(FunctionBuilder *builder, int parent) {
  AllocObjectInstr instr = {
    .base = { .type = INSTR_ALLOC_OBJECT },
    .target_slot = builder->slot_base++,
    .parent_slot = parent
  };
  addinstr(builder, sizeof(instr), (Instr*) &instr);
  return instr.target_slot;
}

int addinstr_alloc_int_object(FunctionBuilder *builder, int value) {
  AllocIntObjectInstr instr = {
    .base = { .type = INSTR_ALLOC_INT_OBJECT },
    .target = (WriteArg) { .kind = ARG_SLOT, .slot = builder->slot_base++ },
    .value = value
  };
  addinstr(builder, sizeof(instr), (Instr*) &instr);
  return instr.target.slot;
}

int addinstr_alloc_bool_object(FunctionBuilder *builder, bool value) {
  AllocBoolObjectInstr instr = {
    .base = { .type = INSTR_ALLOC_BOOL_OBJECT },
    .target = (WriteArg) { .kind = ARG_SLOT, .slot = builder->slot_base++ },
    .value = value
  };
  addinstr(builder, sizeof(instr), (Instr*) &instr);
  return instr.target.slot;
}

int addinstr_alloc_float_object(FunctionBuilder *builder, float value) {
  AllocFloatObjectInstr instr = {
    .base = { .type = INSTR_ALLOC_FLOAT_OBJECT },
    .target = (WriteArg) { .kind = ARG_SLOT, .slot = builder->slot_base++ },
    .value = value
  };
  addinstr(builder, sizeof(instr), (Instr*) &instr);
  return instr.target.slot;
}

int addinstr_alloc_array_object(FunctionBuilder *builder) {
  AllocArrayObjectInstr instr = {
    .base = { .type = INSTR_ALLOC_ARRAY_OBJECT },
    .target = (WriteArg) { .kind = ARG_SLOT, .slot = builder->slot_base++ }
  };
  addinstr(builder, sizeof(instr), (Instr*) &instr);
  return instr.target.slot;
}

int addinstr_alloc_string_object(FunctionBuilder *builder, char *value) {
  AllocStringObjectInstr instr = {
    .base = { .type = INSTR_ALLOC_STRING_OBJECT },
    .target = (WriteArg) { .kind = ARG_SLOT, .slot = builder->slot_base++ },
    .value = value
  };
  addinstr(builder, sizeof(instr), (Instr*) &instr);
  return instr.target.slot;
}

int addinstr_alloc_closure_object(FunctionBuilder *builder, UserFunction *fn) {
  AllocClosureObjectInstr instr = {
    .base = { .type = INSTR_ALLOC_CLOSURE_OBJECT },
    .target = (WriteArg) { .kind = ARG_SLOT, .slot = builder->slot_base++ },
    .fn = fn
  };
  addinstr(builder, sizeof(instr), (Instr*) &instr);
  return instr.target.slot;
}

int addinstr_call(FunctionBuilder *builder, int fn, int this_slot, int *args_ptr, int args_len) {
  int size = sizeof(CallInstr) + sizeof(Arg) * args_len;
  CallInstr *instr = alloca(size);
  instr->base.type = INSTR_CALL;
  instr->base.belongs_to = NULL;
  instr->size = size;
  instr->info.fn = (Arg) { .kind = ARG_SLOT, .slot = fn };
  instr->info.this_arg = (Arg) { .kind = ARG_SLOT, .slot = this_slot };
  instr->info.args_len = args_len;
  instr->info.target = (WriteArg) { .kind = ARG_SLOT, .slot = builder->slot_base++ };
  for (int i = 0; i < args_len; ++i) {
    ((Arg*)(&instr->info + 1))[i] = (Arg) { .kind = ARG_SLOT, .slot = args_ptr[i] };
  }
  addinstr(builder, size, (Instr*) instr);
  return instr->info.target.slot;
}

int addinstr_call0(FunctionBuilder *builder, int fn, int this_slot) {
  return addinstr_call(builder, fn, this_slot, NULL, 0);
}

int addinstr_call1(FunctionBuilder *builder, int fn, int this_slot, int arg0) {
  int *args = alloca(sizeof(int) * 1);
  args[0] = arg0;
  return addinstr_call(builder, fn, this_slot, args, 1);
}

int addinstr_call2(FunctionBuilder *builder, int fn, int this_slot, int arg0, int arg1) {
  int *args = alloca(sizeof(int) * 2);
  args[0] = arg0;
  args[1] = arg1;
  return addinstr_call(builder, fn, this_slot, args, 2);
}

void addinstr_test_branch(FunctionBuilder *builder, int test, int *truebranch, int *falsebranch) {
  TestBranchInstr instr = {
    .base = { .type = INSTR_TESTBR },
    .test = (Arg) { .kind = ARG_SLOT, .slot = test }
  };
  *truebranch = offset_to_instr_about_to_be_added(builder, (char*) &instr, (char*) &instr.true_blk);
  *falsebranch = offset_to_instr_about_to_be_added(builder, (char*) &instr, (char*) &instr.false_blk);
  
  addinstr(builder, sizeof(instr), (Instr*) &instr);
}

int addinstr_phi(FunctionBuilder *builder, int block1, int slot1, int block2, int slot2) {
  PhiInstr instr = {
    .base = { .type = INSTR_PHI },
    .block1 = block1,
    .arg1 = (Arg) { .kind = ARG_SLOT, .slot = slot1 },
    .block2 = block2,
    .arg2 = (Arg) { .kind = ARG_SLOT, .slot = slot2 },
    .target = (WriteArg) { .kind = ARG_SLOT, .slot = builder->slot_base ++ }
  };
  
  addinstr(builder, sizeof(instr), (Instr*) &instr);
  return instr.target.slot;
}

void addinstr_branch(FunctionBuilder *builder, int *branch) {
  BranchInstr instr = {
    .base = { .type = INSTR_BR }
  };
  *branch = offset_to_instr_about_to_be_added(builder, (char*) &instr, (char*) &instr.blk);
  
  addinstr(builder, sizeof(instr), (Instr*) &instr);
}

void addinstr_return(FunctionBuilder *builder, int slot) {
  ReturnInstr instr = {
    .base = { .type = INSTR_RETURN },
    .ret = (Arg) { .kind = ARG_SLOT, .slot = slot }
  };
  
  addinstr(builder, sizeof(instr), (Instr*) &instr);
}

int addinstr_def_refslot(FunctionBuilder *builder, int obj_slot, char *key) {
  int keylen = strlen(key);
  DefineRefslotInstr instr = {
    .base = { .type = INSTR_DEFINE_REFSLOT },
    .obj_slot = obj_slot,
    .key_ptr = key,
    .key_len = keylen,
    .key_hash = hash(key, keylen),
    .target_refslot = builder->refslot_base ++
  };
  
  addinstr(builder, sizeof(instr), (Instr*) &instr);
  return instr.target_refslot;
}

void addinstr_move(FunctionBuilder *builder, Arg source, WriteArg target) {
  MoveInstr instr = {
    .base = { .type = INSTR_MOVE },
    .source = source,
    .target = target
  };
  
  addinstr(builder, sizeof(instr), (Instr*) &instr);
}

UserFunction *build_function(FunctionBuilder *builder) {
  assert(builder->block_terminated);
  if (builder->body.blocks_len == 0) { fprintf(stderr, "Built an invalid function!\n"); abort(); }
  UserFunction *fn = malloc(sizeof(UserFunction));
  fn->arity = builder->arglist_len;
  fn->variadic_tail = builder->variadic_tail;
  fn->slots = builder->slot_base;
  fn->refslots = builder->refslot_base;
  fn->name = builder->name;
  fn->body = builder->body;
  fn->is_method = false;
  fn->non_ssa = false;
  fn->optimized = false;
  return fn;
}
