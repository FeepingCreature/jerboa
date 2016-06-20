#ifndef VM_BUILDER_H
#define VM_BUILDER_H

#include <stddef.h>

#include "object.h"

typedef struct {
  char *name;
  
  char **arglist_ptr;
  int arglist_len;
  
  int scope;
  int slot_base;
  
  bool block_terminated;
  
  FileRange *current_range;
  
  FunctionBody body;
} FunctionBuilder;

// instrs_ptr gets reallocated, so save pointer as an offset
typedef struct {
  int block;
  ptrdiff_t distance;
} IntVarRef;

void record_start(char *text, FileRange *range);

void record_end(char *text, FileRange *range);

void use_range_start(FunctionBuilder *builder, FileRange *range);

void use_range_end(FunctionBuilder *builder, FileRange *range);

FileRange *alloc_and_record_start(char *text);

int new_block(FunctionBuilder *builder);

void terminate(FunctionBuilder *builder);

void addinstr(FunctionBuilder *builder, int size, Instr *instr);

void set_int_var(FunctionBuilder *builder, IntVarRef ref, int value);

int addinstr_access(FunctionBuilder *builder, int obj_slot, int key_slot);

void addinstr_assign(FunctionBuilder *builder, int obj, int key_slot, int slot, AssignType type);

void addinstr_close_object(FunctionBuilder *builder, int obj);

int addinstr_get_context(FunctionBuilder *builder);

int addinstr_alloc_object(FunctionBuilder *builder, int parent);

int addinstr_alloc_int_object(FunctionBuilder *builder, int ctxslot, int value);

int addinstr_alloc_float_object(FunctionBuilder *builder, int ctxslot, float value);

int addinstr_alloc_array_object(FunctionBuilder *builder, int ctxslot);

int addinstr_alloc_string_object(FunctionBuilder *builder, int ctxslot, char *value);

int addinstr_alloc_closure_object(FunctionBuilder *builder, int ctxslot, UserFunction *fn);

int addinstr_call(FunctionBuilder *builder, int fn, int this_slot, int *args_ptr, int args_len);

int addinstr_call0(FunctionBuilder *builder, int fn, int this_slot);

int addinstr_call1(FunctionBuilder *builder, int fn, int this_slot, int arg0);

int addinstr_call2(FunctionBuilder *builder, int fn, int this_slot, int arg0, int arg1);

void addinstr_test_branch(FunctionBuilder *builder, int test, IntVarRef *truebranch, IntVarRef *falsebranch);

void addinstr_branch(FunctionBuilder *builder, IntVarRef *branch);

void addinstr_return(FunctionBuilder *builder, int slot);

UserFunction *build_function(FunctionBuilder *builder);

#endif
