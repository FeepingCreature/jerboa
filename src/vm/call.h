#ifndef VM_CALL_H
#define VM_CALL_H

#include "object.h"

void call_function(VMState *state, Object *context, UserFunction *fn, CallInfo *info);

Value make_closure_fn(VMState *state, Object *context, UserFunction *fn);

bool setup_call(VMState *state, CallInfo *info);

#endif
