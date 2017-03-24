#include "vm/builder.h"

#include "win32_compat.h"

#include "core.h"
#include "util.h"
#include "parser.h"

void record_start(char *text, FileRange *range) {
  eat_filler(&text); // record at the start of the [whatever], not on the end of the previous
  range->text_from = text;
}

void record_end(char *text, FileRange *range) {
  range->text_len = text - range->text_from;
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
  addinstr_return(builder, (Slot) {0});
}

FileRange **instr_belongs_to_p(FunctionBody *body, Instr *instr) {
  int offset = (char*) instr - (char*) body->instrs_ptr;
  assert(offset >= 0 && offset < (char*) body->instrs_ptr_end - (char*) body->instrs_ptr);
  return (FileRange**)((char*) body->ranges_ptr + offset);
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
  
  int new_len_ranges = current_len + sizeof(FileRange*);
  body->ranges_ptr = realloc(body->ranges_ptr, new_len_ranges);
  *instr_belongs_to_p(body, new_instr) = builder->current_range;
  
  new_instr->context_slot = builder->scope;
  if (instr->type == INSTR_BR || instr->type == INSTR_TESTBR || instr->type == INSTR_RETURN) {
    builder->block_terminated = true;
  }
}

#include <stdio.h>
void addinstr_like(FunctionBuilder *builder, FunctionBody *body, Instr *basis, int size, Instr *instr) {
  Slot backup = builder->scope;
  use_range_start(builder, *instr_belongs_to_p(body, basis));
  builder->scope = basis->context_slot;
  addinstr(builder, size, instr);
  use_range_end(builder, *instr_belongs_to_p(body, basis));
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

Slot addinstr_access(FunctionBuilder *builder, Slot obj_slot, Slot key_slot) {
  AccessInstr instr = {
    .base = { .type = INSTR_ACCESS },
    .obj = (Arg) { .kind = ARG_SLOT, .slot = obj_slot },
    .key = (Arg) { .kind = ARG_SLOT, .slot = key_slot },
    .target = (WriteArg) { .kind = ARG_SLOT, .slot = { .index = builder->slot_base++ } }
  };
  addinstr(builder, sizeof(instr), (Instr*) &instr);
  return instr.target.slot;
}

void addinstr_assign(FunctionBuilder *builder, Slot obj, Slot key_slot, Slot slot, AssignType type) {
  AssignInstr instr = {
    .base = { .type = INSTR_ASSIGN },
    .obj = (Arg) { .kind = ARG_SLOT, .slot = obj },
    .value = (Arg) { .kind = ARG_SLOT, .slot = slot },
    .key = (Arg) { .kind = ARG_SLOT, .slot = key_slot },
    .target_slot = (Slot) { .index = builder->slot_base++ },
    .type = type
  };
  addinstr(builder, sizeof(instr), (Instr*) &instr);
}

Slot addinstr_key_in_obj(FunctionBuilder *builder, Slot key_slot, Slot obj_slot) {
  KeyInObjInstr instr = {
    .base = { .type = INSTR_KEY_IN_OBJ },
    .obj = (Arg) { .kind = ARG_SLOT, .slot = obj_slot },
    .key = (Arg) { .kind = ARG_SLOT, .slot = key_slot },
    .target = (WriteArg) { .kind = ARG_SLOT, .slot = { .index = builder->slot_base++ } }
  };
  addinstr(builder, sizeof(instr), (Instr*) &instr);
  return instr.target.slot;
}

Slot addinstr_identical(FunctionBuilder *builder, Slot slot1, Slot slot2) {
  IdenticalInstr instr = {
    .base = { .type = INSTR_IDENTICAL },
    .obj1 = (Arg) { .kind = ARG_SLOT, .slot = slot1 },
    .obj2 = (Arg) { .kind = ARG_SLOT, .slot = slot2 },
    .target = (WriteArg) { .kind = ARG_SLOT, .slot = { .index = builder->slot_base++ } }
  };
  addinstr(builder, sizeof(instr), (Instr*) &instr);
  return instr.target.slot;
}

Slot addinstr_instanceof(FunctionBuilder *builder, Slot obj_slot, Slot proto_slot) {
  InstanceofInstr instr = {
    .base = { .type = INSTR_INSTANCEOF },
    .obj = (Arg) { .kind = ARG_SLOT, .slot = obj_slot },
    .proto = (Arg) { .kind = ARG_SLOT, .slot = proto_slot },
    .target = (WriteArg) { .kind = ARG_SLOT, .slot = { .index = builder->slot_base++ } }
  };
  addinstr(builder, sizeof(instr), (Instr*) &instr);
  return instr.target.slot;
}

void addinstr_set_constraint(FunctionBuilder *builder, Slot obj_slot, Slot key_slot, Slot cons_slot) {
  SetConstraintInstr instr = {
    .base = { .type = INSTR_SET_CONSTRAINT },
    .obj = (Arg) { .kind = ARG_SLOT, .slot = obj_slot },
    .key = (Arg) { .kind = ARG_SLOT, .slot = key_slot },
    .constraint = (Arg) { .kind = ARG_SLOT, .slot = cons_slot }
  };
  addinstr(builder, sizeof(instr), (Instr*) &instr);
}

void addinstr_close_object(FunctionBuilder *builder, Slot obj) {
  CloseObjectInstr instr = {
    .base = { .type = INSTR_CLOSE_OBJECT },
    .slot = obj
  };
  addinstr(builder, sizeof(instr), (Instr*) &instr);
}

void addinstr_freeze_object(FunctionBuilder *builder, Slot obj) {
  FreezeObjectInstr instr = {
    .base = { .type = INSTR_FREEZE_OBJECT },
    .slot = obj
  };
  addinstr(builder, sizeof(instr), (Instr*) &instr);
}

Slot addinstr_alloc_object(FunctionBuilder *builder, Slot parent) {
  AllocObjectInstr instr = {
    .base = { .type = INSTR_ALLOC_OBJECT },
    .target_slot = (Slot) { .index = builder->slot_base++ },
    .parent_slot = parent,
    .alloc_stack = false
  };
  addinstr(builder, sizeof(instr), (Instr*) &instr);
  return instr.target_slot;
}

Slot addinstr_alloc_int_object(FunctionBuilder *builder, int value) {
  AllocIntObjectInstr instr = {
    .base = { .type = INSTR_ALLOC_INT_OBJECT },
    .target = (WriteArg) { .kind = ARG_SLOT, .slot = { .index = builder->slot_base++ } },
    .value = value
  };
  addinstr(builder, sizeof(instr), (Instr*) &instr);
  return instr.target.slot;
}

Slot addinstr_alloc_bool_object(FunctionBuilder *builder, bool value) {
  AllocBoolObjectInstr instr = {
    .base = { .type = INSTR_ALLOC_BOOL_OBJECT },
    .target = (WriteArg) { .kind = ARG_SLOT, .slot = { .index = builder->slot_base++ } },
    .value = value
  };
  addinstr(builder, sizeof(instr), (Instr*) &instr);
  return instr.target.slot;
}

Slot addinstr_alloc_float_object(FunctionBuilder *builder, float value) {
  AllocFloatObjectInstr instr = {
    .base = { .type = INSTR_ALLOC_FLOAT_OBJECT },
    .target = (WriteArg) { .kind = ARG_SLOT, .slot = { .index = builder->slot_base++ } },
    .value = value
  };
  addinstr(builder, sizeof(instr), (Instr*) &instr);
  return instr.target.slot;
}

Slot addinstr_alloc_array_object(FunctionBuilder *builder) {
  AllocArrayObjectInstr instr = {
    .base = { .type = INSTR_ALLOC_ARRAY_OBJECT },
    .target = (WriteArg) { .kind = ARG_SLOT, .slot = { .index = builder->slot_base++ } }
  };
  addinstr(builder, sizeof(instr), (Instr*) &instr);
  return instr.target.slot;
}

Slot addinstr_alloc_string_object(FunctionBuilder *builder, char *value) {
  AllocStringObjectInstr instr = {
    .base = { .type = INSTR_ALLOC_STRING_OBJECT },
    .target = (WriteArg) { .kind = ARG_SLOT, .slot = { .index = builder->slot_base++ } },
    .value = value
  };
  addinstr(builder, sizeof(instr), (Instr*) &instr);
  return instr.target.slot;
}

Slot addinstr_alloc_closure_object(FunctionBuilder *builder, UserFunction *fn) {
  AllocClosureObjectInstr instr = {
    .base = { .type = INSTR_ALLOC_CLOSURE_OBJECT },
    .target = (WriteArg) { .kind = ARG_SLOT, .slot = { .index = builder->slot_base++ } },
    .fn = fn
  };
  addinstr(builder, sizeof(instr), (Instr*) &instr);
  return instr.target.slot;
}

Slot addinstr_test(FunctionBuilder *builder, Slot value_slot) {
  TestInstr instr = {
    .base = { .type = INSTR_TEST },
    .value = { .kind = ARG_SLOT, .slot = value_slot },
    .target = { .kind = ARG_SLOT, .slot = { .index = builder->slot_base++ } }
  };
  addinstr(builder, sizeof(instr), (Instr*) &instr);
  return instr.target.slot;
}

Slot addinstr_call(FunctionBuilder *builder, Slot fn, Slot this_slot, Slot *args_ptr, int args_len) {
  int size = sizeof(CallInstr) + sizeof(Arg) * args_len;
  CallInstr *instr = alloca(size);
  instr->base.type = INSTR_CALL;
  instr->size = size;
  instr->info.fn = (Arg) { .kind = ARG_SLOT, .slot = fn };
  instr->info.this_arg = (Arg) { .kind = ARG_SLOT, .slot = this_slot };
  instr->info.args_len = args_len;
  instr->info.target = (WriteArg) { .kind = ARG_SLOT, .slot = { .index = builder->slot_base++ } };
  for (int i = 0; i < args_len; ++i) {
    ((Arg*)(&instr->info + 1))[i] = (Arg) { .kind = ARG_SLOT, .slot = args_ptr[i] };
  }
  addinstr(builder, size, (Instr*) instr);
  return instr->info.target.slot;
}

Slot addinstr_call0(FunctionBuilder *builder, Slot fn, Slot this_slot) {
  return addinstr_call(builder, fn, this_slot, NULL, 0);
}

Slot addinstr_call1(FunctionBuilder *builder, Slot fn, Slot this_slot, Slot arg0) {
  Slot *args = alloca(sizeof(Slot) * 1);
  args[0] = arg0;
  return addinstr_call(builder, fn, this_slot, args, 1);
}

Slot addinstr_call2(FunctionBuilder *builder, Slot fn, Slot this_slot, Slot arg0, Slot arg1) {
  Slot *args = alloca(sizeof(Slot) * 2);
  args[0] = arg0;
  args[1] = arg1;
  return addinstr_call(builder, fn, this_slot, args, 2);
}

void addinstr_test_branch(FunctionBuilder *builder, Slot test, int *truebranch, int *falsebranch) {
  TestBranchInstr instr = {
    .base = { .type = INSTR_TESTBR },
    .test = { .kind = ARG_SLOT, .slot = test }
  };
  *truebranch = offset_to_instr_about_to_be_added(builder, (char*) &instr, (char*) &instr.true_blk);
  *falsebranch = offset_to_instr_about_to_be_added(builder, (char*) &instr, (char*) &instr.false_blk);
  
  // no need to have a live context here, since
  // we're just branching on a slot, never a refslot
  Slot backup = builder->scope; builder->scope = (Slot) {0};
  addinstr(builder, sizeof(instr), (Instr*) &instr);
  builder->scope = backup;
}

Slot addinstr_phi(FunctionBuilder *builder, int block1, Slot slot1, int block2, Slot slot2) {
  PhiInstr instr = {
    .base = { .type = INSTR_PHI },
    .block1 = block1,
    .arg1 = (Arg) { .kind = ARG_SLOT, .slot = slot1 },
    .block2 = block2,
    .arg2 = (Arg) { .kind = ARG_SLOT, .slot = slot2 },
    .target = (WriteArg) { .kind = ARG_SLOT, .slot = { .index = builder->slot_base ++ } }
  };
  
  addinstr(builder, sizeof(instr), (Instr*) &instr);
  return instr.target.slot;
}

void addinstr_branch(FunctionBuilder *builder, int *branch) {
  BranchInstr instr = {
    .base = { .type = INSTR_BR }
  };
  *branch = offset_to_instr_about_to_be_added(builder, (char*) &instr, (char*) &instr.blk);
  
  // no need to have a live context here
  Slot backup = builder->scope; builder->scope = (Slot) {0};
  addinstr(builder, sizeof(instr), (Instr*) &instr);
  builder->scope = backup;
}

void addinstr_return(FunctionBuilder *builder, Slot slot) {
  ReturnInstr instr = {
    .base = { .type = INSTR_RETURN },
    .ret = (Arg) { .kind = ARG_SLOT, .slot = slot }
  };
  
  // no need to have a live context here
  Slot backup = builder->scope; builder->scope = (Slot) {0};
  addinstr(builder, sizeof(instr), (Instr*) &instr);
  builder->scope = backup;
}

Refslot addinstr_def_refslot(FunctionBuilder *builder, Slot obj_slot, const char *key_ptr, size_t key_len) {
  DefineRefslotInstr instr = {
    .base = { .type = INSTR_DEFINE_REFSLOT },
    .obj_slot = obj_slot,
    .key = prepare_key(key_ptr, key_len),
    .target_refslot = { .index = builder->refslot_base ++ }
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

LoopRecord *open_loop(FunctionBuilder *builder, char *name) {
  LoopRecord *new_record = calloc(sizeof(LoopRecord), 1);
  new_record->label = name;
  new_record->prev_loop = builder->loops;
  builder->loops = new_record;
  return new_record;
}

void close_loop(FunctionBuilder *builder, LoopRecord *record, int brk_blk, int cont_blk) {
  if (record != builder->loops) {
    fprintf(stderr, "loop open/close order mismatch\n");
    abort();
  }
  builder->loops = builder->loops->prev_loop;
  for (int i = 0; i < record->branches_brk_len; i++) {
    set_int_var(builder, record->branches_brk_ptr[i], brk_blk);
  }
  for (int i = 0; i < record->branches_cont_len; i++) {
    set_int_var(builder, record->branches_cont_ptr[i], cont_blk);
  }
}

char *loop_contbrk(FunctionBuilder *builder, char *name, bool is_break) {
  if (!builder) return NULL;
  
  int **branches_ptr_p, *branches_len_p;
  LoopRecord *my_record = builder->loops; // innermost loop
  if (!my_record) {
    return "Not inside a loop";
  }
  if (name) {
    while (my_record && (!my_record->label || strcmp(my_record->label, name) != 0)) {
      my_record = my_record->prev_loop;
    }
    if (!my_record) {
      return my_asprintf("No loop found with label '%s'", name);
    }
  }
  if (is_break) {
    branches_ptr_p = &my_record->branches_brk_ptr;
    branches_len_p = &my_record->branches_brk_len;
  } else {
    branches_ptr_p = &my_record->branches_cont_ptr;
    branches_len_p = &my_record->branches_cont_len;
  }
  
  *branches_ptr_p = realloc(*branches_ptr_p, ++*branches_len_p);
  addinstr_branch(builder, &(*branches_ptr_p)[*branches_len_p - 1]);
  new_block(builder);
  
  return NULL;
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
  fn->resolved = false;
  fn->num_optimized = 0;
  fn->proposed_jit_fn = fn->opt_jit_fn = NULL;
  return fn;
}
