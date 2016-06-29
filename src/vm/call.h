#ifndef VM_CALL_H
#define VM_CALL_H

#include "object.h"

void call_function(VMState *state, Object *context, UserFunction *fn, Object **args_ptr, int args_len);

void function_handler(VMState *state, Object *thisptr, Object *fn, Object **args_ptr, int args_len);

void method_handler(VMState *state, Object *thisptr, Object *fn, Object **args_ptr, int args_len);

Object *alloc_closure_fn(VMState *state, Object *context, UserFunction *fn);

#endif
