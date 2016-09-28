#ifndef JERBOA_VM_CALL_H
#define JERBOA_VM_CALL_H

#include "object.h"

void call_function(VMState *state, Object *context, UserFunction *fn, CallInfo *info);

Value make_closure_fn(VMState *state, Object *context, UserFunction *fn);

bool setup_call(VMState *state, CallInfo *info);

#endif
