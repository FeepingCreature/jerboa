#ifndef VM_BUILDER_H
#define VM_BUILDER_H

#include "object.h"

typedef struct {
  char *name;
  
  char **arglist_ptr;
  int arglist_len;
  
  int scope;
  int slot_base;
  
  bool block_terminated;
  
  FunctionBody body;
} FunctionBuilder;

int new_block(FunctionBuilder *builder);

void terminate(FunctionBuilder *builder);

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

void addinstr_test_branch(FunctionBuilder *builder, int test, int **truebranch, int **falsebranch);

void addinstr_branch(FunctionBuilder *builder, int **branch);

void addinstr_return(FunctionBuilder *builder, int slot);

UserFunction *build_function(FunctionBuilder *builder);

#endif
