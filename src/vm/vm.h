#ifndef VM_VM_H
#define VM_VM_H

#include "object.h"

#define VM_ASSERT(cond, ...) if (!(cond) && (vm_error(state, __VA_ARGS__), true)) return

Callframe *vm_alloc_frame(VMState *state, int slots);

void vm_remove_frame(VMState *state);

void vm_error(VMState *state, char *fmt, ...);

void vm_print_backtrace(VMState *state);

void vm_run(VMState *state);

#endif
