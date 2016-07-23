#ifndef VM_CALL_H
#define VM_CALL_H

#include "object.h"

void call_function(VMState *state, Object *context, UserFunction *fn, Value *args_ptr, int args_len);

Value make_closure_fn(VMState *state, Object *context, UserFunction *fn);

bool setup_call(VMState *state, Value thisval, Value fn, Value *args_ptr, int args_len);

#endif
