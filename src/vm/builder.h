#ifndef JERBOA_VM_BUILDER_H
#define JERBOA_VM_BUILDER_H

#include <stddef.h>

#include "object.h"

typedef struct _LoopRecord LoopRecord;
struct _LoopRecord {
  char *label;
  int *branches_cont_ptr, *branches_brk_ptr;
  int branches_cont_len, branches_brk_len;
  LoopRecord *prev_loop;
};

typedef struct {
  // used to enable foo["bar"] = function() { } name hinting
  Slot string_literal_hint_slot; char *string_literal_hint;
  char *fun_name_hint_pos, *fun_name_hint;
} ContextHints;

typedef struct {
  char *name;
  
  char **arglist_ptr;
  int arglist_len;
  bool variadic_tail;
  
  Slot scope;
  int slot_base; // base-1; 0 is reserved for "null"
  int refslot_base;
  
  bool block_terminated;
  LoopRecord *loops;
  
  FileRange *current_range;
  ContextHints hints; // used for internal pattern-based hacks, like ["foo"]=function being tagged as foo
  
  FunctionBody body;
} FunctionBuilder;

LoopRecord *open_loop(FunctionBuilder *builder, char *name);

void close_loop(FunctionBuilder *builder, LoopRecord *record, int brk_blk, int cont_blk);

char *loop_contbrk(FunctionBuilder *builder, char *name, bool is_break);

void record_start(char *text, FileRange *range);

void record_end(char *text, FileRange *range);

void use_range_start(FunctionBuilder *builder, FileRange *range);

void use_range_end(FunctionBuilder *builder, FileRange *range);

FileRange *alloc_and_record_start(char *text);

int new_block(FunctionBuilder *builder);

int get_block(FunctionBuilder *builder);

void terminate(FunctionBuilder *builder);

void addinstr(FunctionBuilder *builder, int size, Instr *instr);

void addinstr_like(FunctionBuilder *builder, FunctionBody *body, Instr *basis, int size, Instr *instr);

void set_int_var(FunctionBuilder *builder, int offset, int value);

Slot addinstr_get_root(FunctionBuilder *builder);

Slot addinstr_access(FunctionBuilder *builder, Slot obj_slot, Slot key_slot);

void addinstr_assign(FunctionBuilder *builder, Slot obj, Slot key_slot, Slot slot, AssignType type);

Slot addinstr_key_in_obj(FunctionBuilder *builder, Slot key_slot, Slot obj_slot);

Slot addinstr_identical(FunctionBuilder *builder, Slot slot1, Slot slot2);

Slot addinstr_instanceof(FunctionBuilder *builder, Slot obj_slot, Slot proto_slot);

void addinstr_set_constraint(FunctionBuilder *builder, Slot obj_slot, Slot key_slot, Slot cons_slot);

void addinstr_close_object(FunctionBuilder *builder, Slot obj);

void addinstr_freeze_object(FunctionBuilder *builder, Slot obj);

Slot addinstr_get_context(FunctionBuilder *builder);

void addinstr_set_context(FunctionBuilder *builder, Slot obj);

Slot addinstr_alloc_object(FunctionBuilder *builder, Slot parent);

Slot addinstr_alloc_int_object(FunctionBuilder *builder, int value);

Slot addinstr_alloc_bool_object(FunctionBuilder *builder, bool value);

Slot addinstr_alloc_float_object(FunctionBuilder *builder, float value);

Slot addinstr_alloc_array_object(FunctionBuilder *builder);

Slot addinstr_alloc_string_object(FunctionBuilder *builder, char *value);

Slot addinstr_alloc_closure_object(FunctionBuilder *builder, UserFunction *fn);

Slot addinstr_test(FunctionBuilder *builder, Slot value_slot);

Slot addinstr_call(FunctionBuilder *builder, Slot fn, Slot this_slot, Slot *args_ptr, int args_len);

Slot addinstr_call0(FunctionBuilder *builder, Slot fn, Slot this_slot);

Slot addinstr_call1(FunctionBuilder *builder, Slot fn, Slot this_slot, Slot arg0);

Slot addinstr_call2(FunctionBuilder *builder, Slot fn, Slot this_slot, Slot arg0, Slot arg1);

void addinstr_test_branch(FunctionBuilder *builder, Slot test, int *truebranch, int *falsebranch);

void addinstr_branch(FunctionBuilder *builder, int *branch);

Slot addinstr_phi(FunctionBuilder *builder, int block1, Slot slot1, int block2, Slot slot2);

void addinstr_return(FunctionBuilder *builder, Slot slot);

Refslot addinstr_def_refslot(FunctionBuilder *builder, Slot obj_slot, const char *key_ptr, size_t key_len);

void addinstr_move(FunctionBuilder *builder, Arg source, WriteArg target);

UserFunction *build_function(FunctionBuilder *builder);

#endif
