#ifndef VM_CALL_H
#define VM_CALL_H

#include "object.h"

void call_function(VMState *state, Object *context, UserFunction *fn, Object **args_ptr, int args_len);

Object *alloc_closure_fn(VMState *state, Object *context, UserFunction *fn);

bool setup_call(VMState *state, Object *thisptr, Object *fn, Object **args_ptr, int args_len);

#endif
