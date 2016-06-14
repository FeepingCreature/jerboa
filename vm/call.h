#ifndef VM_CALL_H
#define VM_CALL_H

#include "object.h"

#define VM_ASSERT(cond, ...) if (!(cond) && (vm_error(state, __VA_ARGS__), true)) return
int cyclecount;

void call_function(VMState *state, Object *context, UserFunction *fn, Object **args_ptr, int args_len);

void function_handler(VMState *state, Object *thisptr, Object *fn, Object **args_ptr, int args_len);

void method_handler(VMState *state, Object *thisptr, Object *fn, Object **args_ptr, int args_len);

Object *alloc_closure_fn(Object *context, UserFunction *fn);

Callframe *vm_alloc_frame(VMState *state);

void vm_error(VMState *state, char *fmt, ...);

void vm_remove_frame(VMState *state);

void vm_run(VMState *state, Object *root);

#endif
