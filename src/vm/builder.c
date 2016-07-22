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
  AccessInstr instr = {
    .base = {
      .type = INSTR_ACCESS,
      .belongs_to = NULL
    },
    .obj_slot = obj_slot,
    .key_slot = key_slot,
    .target_slot = builder->slot_base++
  };
  addinstr(builder, sizeof(instr), (Instr*) &instr);
  return instr.target_slot;
}

void addinstr_assign(FunctionBuilder *builder, int obj, int key_slot, int slot, AssignType type) {
  AssignInstr instr = {
    .base = {
      .type = INSTR_ASSIGN,
      .belongs_to = NULL
    },
    .obj_slot = obj,
    .value_slot = slot,
    .key_slot = key_slot,
    .type = type
  };
  addinstr(builder, sizeof(instr), (Instr*) &instr);
}

int addinstr_key_in_obj(FunctionBuilder *builder, int key_slot, int obj_slot) {
  KeyInObjInstr instr = {
    .base = {
      .type = INSTR_KEY_IN_OBJ,
      .belongs_to = NULL
    },
    .key_slot = key_slot,
    .obj_slot = obj_slot,
    .target_slot = builder->slot_base++
  };
  addinstr(builder, sizeof(instr), (Instr*) &instr);
  return instr.target_slot;
}

void addinstr_set_constraint(FunctionBuilder *builder, int obj_slot, int key_slot, int cons_slot) {
  SetConstraintInstr instr = {
    .base = {
      .type = INSTR_SET_CONSTRAINT,
      .belongs_to = NULL
    },
    .obj_slot = obj_slot,
    .key_slot = key_slot,
    .constraint_slot = cons_slot
  };
  addinstr(builder, sizeof(instr), (Instr*) &instr);
}

void addinstr_close_object(FunctionBuilder *builder, int obj) {
  CloseObjectInstr instr = {
    .base = {
      .type = INSTR_CLOSE_OBJECT,
      .belongs_to = NULL
    },
    .slot = obj
  };
  addinstr(builder, sizeof(instr), (Instr*) &instr);
}

void addinstr_freeze_object(FunctionBuilder *builder, int obj) {
  FreezeObjectInstr instr = {
    .base = {
      .type = INSTR_FREEZE_OBJECT,
      .belongs_to = NULL
    },
    .slot = obj
  };
  addinstr(builder, sizeof(instr), (Instr*) &instr);
}

int addinstr_alloc_object(FunctionBuilder *builder, int parent) {
  AllocObjectInstr instr = {
    .base = {
      .type = INSTR_ALLOC_OBJECT,
      .belongs_to = NULL
    },
    .target_slot = builder->slot_base++,
    .parent_slot = parent
  };
  addinstr(builder, sizeof(instr), (Instr*) &instr);
  return instr.target_slot;
}

int addinstr_alloc_int_object(FunctionBuilder *builder, int value) {
  AllocIntObjectInstr instr = {
    .base = {
      .type = INSTR_ALLOC_INT_OBJECT,
      .belongs_to = NULL
    },
    .target_slot = builder->slot_base++,
    .value = value
  };
  addinstr(builder, sizeof(instr), (Instr*) &instr);
  return instr.target_slot;
}

int addinstr_alloc_bool_object(FunctionBuilder *builder, bool value) {
  AllocBoolObjectInstr instr = {
    .base = {
      .type = INSTR_ALLOC_BOOL_OBJECT,
      .belongs_to = NULL
    },
    .target_slot = builder->slot_base++,
    .value = value
  };
  addinstr(builder, sizeof(instr), (Instr*) &instr);
  return instr.target_slot;
}

int addinstr_alloc_float_object(FunctionBuilder *builder, float value) {
  AllocFloatObjectInstr instr = {
    .base = {
      .type = INSTR_ALLOC_FLOAT_OBJECT,
      .belongs_to = NULL
    },
    .target_slot = builder->slot_base++,
    .value = value
  };
  addinstr(builder, sizeof(instr), (Instr*) &instr);
  return instr.target_slot;
}

int addinstr_alloc_array_object(FunctionBuilder *builder) {
  AllocArrayObjectInstr instr = {
    .base = {
      .type = INSTR_ALLOC_ARRAY_OBJECT,
      .belongs_to = NULL
    },
    .target_slot = builder->slot_base++
  };
  addinstr(builder, sizeof(instr), (Instr*) &instr);
  return instr.target_slot;
}

int addinstr_alloc_string_object(FunctionBuilder *builder, char *value) {
  AllocStringObjectInstr instr = {
    .base = {
      .type = INSTR_ALLOC_STRING_OBJECT,
      .belongs_to = NULL
    },
    .target_slot = builder->slot_base++,
    .value = value
  };
  addinstr(builder, sizeof(instr), (Instr*) &instr);
  return instr.target_slot;
}

int addinstr_alloc_closure_object(FunctionBuilder *builder, UserFunction *fn) {
  AllocClosureObjectInstr instr = {
    .base = {
      .type = INSTR_ALLOC_CLOSURE_OBJECT,
      .belongs_to = NULL
    },
    .target_slot = builder->slot_base++,
    .fn = fn
  };
  addinstr(builder, sizeof(instr), (Instr*) &instr);
  return instr.target_slot;
}

int addinstr_call(FunctionBuilder *builder, int fn, int this_slot, int *args_ptr, int args_len) {
  CallInstr *instr1 = alloca(sizeof(CallInstr) + sizeof(int) * args_len);
  instr1->base.type = INSTR_CALL;
  instr1->base.belongs_to = NULL;
  instr1->function_slot = fn;
  instr1->this_slot = this_slot;
  instr1->args_length = args_len;
  memcpy((int*)(instr1 + 1), args_ptr, sizeof(int) * args_len);
  addinstr(builder, sizeof(*instr1) + sizeof(int) * args_len, (Instr*) instr1);
  
  SaveResultInstr instr2 = {
    .base = {
      .type = INSTR_SAVE_RESULT,
      .belongs_to = NULL
    },
    .target_slot = builder->slot_base++
  };
  addinstr(builder, sizeof(instr2), (Instr*) &instr2);
  return instr2.target_slot;
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
    .base = {
      .type = INSTR_TESTBR,
      .belongs_to = NULL
    },
    .test_slot = test
  };
  *truebranch = offset_to_instr_about_to_be_added(builder, (char*) &instr, (char*) &instr.true_blk);
  *falsebranch = offset_to_instr_about_to_be_added(builder, (char*) &instr, (char*) &instr.false_blk);
  
  addinstr(builder, sizeof(instr), (Instr*) &instr);
}

int addinstr_phi(FunctionBuilder *builder, int block1, int slot1, int block2, int slot2) {
  PhiInstr instr = {
    .base = {
      .type = INSTR_PHI,
      .belongs_to = NULL
    },
    .block1 = block1,
    .slot1 = slot1,
    .block2 = block2,
    .slot2 = slot2,
    .target_slot = builder->slot_base ++
  };
  
  addinstr(builder, sizeof(instr), (Instr*) &instr);
  return instr.target_slot;
}

void addinstr_branch(FunctionBuilder *builder, int *branch) {
  BranchInstr instr = {
    .base = {
      .type = INSTR_BR,
      .belongs_to = NULL
    }
  };
  *branch = offset_to_instr_about_to_be_added(builder, (char*) &instr, (char*) &instr.blk);
  
  addinstr(builder, sizeof(instr), (Instr*) &instr);
}

void addinstr_return(FunctionBuilder *builder, int slot) {
  ReturnInstr instr = {
    .base = {
      .type = INSTR_RETURN,
      .belongs_to = NULL
    },
    .ret_slot = slot
  };
  
  addinstr(builder, sizeof(instr), (Instr*) &instr);
}

int addinstr_def_refslot(FunctionBuilder *builder, int obj_slot, char *key) {
  int keylen = strlen(key);
  DefineRefslotInstr instr = {
    .base = {
      .type = INSTR_DEFINE_REFSLOT,
      .belongs_to = NULL
    },
    .obj_slot = obj_slot,
    .key_ptr = key,
    .key_len = keylen,
    .key_hash = hash(key, keylen),
    .target_refslot = builder->refslot_base ++
  };
  
  addinstr(builder, sizeof(instr), (Instr*) &instr);
  return instr.target_refslot;
}

void addinstr_read_refslot(FunctionBuilder *builder, int source_refslot, int target_slot, char *opt_info) {
  ReadRefslotInstr instr = {
    .base = {
      .type = INSTR_READ_REFSLOT,
      .belongs_to = NULL
    },
    .source_refslot = source_refslot,
    .target_slot = target_slot,
    .opt_info = opt_info
  };
  
  addinstr(builder, sizeof(instr), (Instr*) &instr);
}

void addinstr_write_refslot(FunctionBuilder *builder, int source_slot, int target_refslot, char *opt_info) {
  WriteRefslotInstr instr = {
    .base = {
      .type = INSTR_WRITE_REFSLOT,
      .belongs_to = NULL
    },
    .source_slot = source_slot,
    .target_refslot = target_refslot,
    .opt_info = opt_info
  };
  
  addinstr(builder, sizeof(instr), (Instr*) &instr);
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
  fn->non_ssa = false;
  fn->optimized = false;
  return fn;
}
