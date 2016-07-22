#ifndef VM_BUILDER_H
#define VM_BUILDER_H

#include <stddef.h>

#include "object.h"

typedef struct {
  char *name;
  
  char **arglist_ptr;
  int arglist_len;
  bool variadic_tail;
  
  int scope;
  int slot_base; // base-1; 0 is reserved for "null"
  int refslot_base;
  
  bool block_terminated;
  
  FileRange *current_range;
  
  FunctionBody body;
} FunctionBuilder;

void record_start(char *text, FileRange *range);

void record_end(char *text, FileRange *range);

void use_range_start(FunctionBuilder *builder, FileRange *range);

void use_range_end(FunctionBuilder *builder, FileRange *range);

FileRange *alloc_and_record_start(char *text);

int new_block(FunctionBuilder *builder);

int get_block(FunctionBuilder *builder);

void terminate(FunctionBuilder *builder);

void addinstr(FunctionBuilder *builder, int size, Instr *instr);

void addinstr_like(FunctionBuilder *builder, Instr *basis, int size, Instr *instr);

void set_int_var(FunctionBuilder *builder, int offset, int value);

int addinstr_access(FunctionBuilder *builder, int obj_slot, int key_slot);

void addinstr_assign(FunctionBuilder *builder, int obj, int key_slot, int slot, AssignType type);

int addinstr_key_in_obj(FunctionBuilder *builder, int key_slot, int obj_slot);

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

int addinstr_call(FunctionBuilder *builder, int fn, int this_slot, int *args_ptr, int args_len);

int addinstr_call0(FunctionBuilder *builder, int fn, int this_slot);

int addinstr_call1(FunctionBuilder *builder, int fn, int this_slot, int arg0);

int addinstr_call2(FunctionBuilder *builder, int fn, int this_slot, int arg0, int arg1);

void addinstr_test_branch(FunctionBuilder *builder, int test, int *truebranch, int *falsebranch);

void addinstr_branch(FunctionBuilder *builder, int *branch);

int addinstr_phi(FunctionBuilder *builder, int block1, int slot1, int block2, int slot2);

void addinstr_return(FunctionBuilder *builder, int slot);

int addinstr_def_refslot(FunctionBuilder *builder, int obj_slot, char *key);

void addinstr_read_refslot(FunctionBuilder *builder, int source_refslot, int target_slot, char *opt_info);

void addinstr_write_refslot(FunctionBuilder *builder, int source_slot, int target_refslot, char *opt_info);

UserFunction *build_function(FunctionBuilder *builder);

#endif
