#ifndef VM_BUILDER_H
#define VM_BUILDER_H

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
  char *name;
  
  char **arglist_ptr;
  int arglist_len;
  bool variadic_tail;
  
  int scope;
  int slot_base; // base-1; 0 is reserved for "null"
  int refslot_base;
  
  bool block_terminated;
  LoopRecord *loops;
  
  FileRange *current_range;
  
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

int addinstr_get_root(FunctionBuilder *builder);

int addinstr_access(FunctionBuilder *builder, int obj_slot, int key_slot);

void addinstr_assign(FunctionBuilder *builder, int obj, int key_slot, int slot, AssignType type);

int addinstr_key_in_obj(FunctionBuilder *builder, int key_slot, int obj_slot);

int addinstr_identical(FunctionBuilder *builder, int slot1, int slot2);

int addinstr_instanceof(FunctionBuilder *builder, int obj_slot, int proto_slot);

void addinstr_set_constraint(FunctionBuilder *builder, int obj_slot, int key_slot, int cons_slot);

void addinstr_close_object(FunctionBuilder *builder, int obj);

void addinstr_freeze_object(FunctionBuilder *builder, int obj);

int addinstr_get_context(FunctionBuilder *builder);

void addinstr_set_context(FunctionBuilder *builder, int obj);

int addinstr_alloc_object(FunctionBuilder *builder, int parent);

int addinstr_alloc_int_object(FunctionBuilder *builder, int value);

int addinstr_alloc_bool_object(FunctionBuilder *builder, bool value);

int addinstr_alloc_float_object(FunctionBuilder *builder, float value);

int addinstr_alloc_array_object(FunctionBuilder *builder);

int addinstr_alloc_string_object(FunctionBuilder *builder, char *value);

int addinstr_alloc_closure_object(FunctionBuilder *builder, UserFunction *fn);

int addinstr_test(FunctionBuilder *builder, int value_slot);

int addinstr_call(FunctionBuilder *builder, int fn, int this_slot, int *args_ptr, int args_len);

int addinstr_call0(FunctionBuilder *builder, int fn, int this_slot);

int addinstr_call1(FunctionBuilder *builder, int fn, int this_slot, int arg0);

int addinstr_call2(FunctionBuilder *builder, int fn, int this_slot, int arg0, int arg1);

void addinstr_test_branch(FunctionBuilder *builder, int test, int *truebranch, int *falsebranch);

void addinstr_branch(FunctionBuilder *builder, int *branch);

int addinstr_phi(FunctionBuilder *builder, int block1, int slot1, int block2, int slot2);

void addinstr_return(FunctionBuilder *builder, int slot);

int addinstr_def_refslot(FunctionBuilder *builder, int obj_slot, const char *key_ptr, size_t key_len);

void addinstr_move(FunctionBuilder *builder, Arg source, WriteArg target);

UserFunction *build_function(FunctionBuilder *builder);

#endif
