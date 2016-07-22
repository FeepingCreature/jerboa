#ifndef VM_VM_H
#define VM_VM_H

#include "object.h"

#define VM_ASSERT(cond, ...) if (UNLIKELY(!(cond)) && (vm_error(state, __VA_ARGS__), true)) return

#ifndef NDEBUG
#define VM_ASSERT_DEBUG(cond, ...) VM_ASSERT(cond, __VA_ARGS__)
#else
#define VM_ASSERT_DEBUG(cond, ...) (void) 0
#endif

void *vm_stack_alloc(VMState *state, int size);

void *vm_stack_alloc_uninitialized(VMState *state, int size);

void vm_stack_free(VMState *state, void *ptr, int size);

void vm_alloc_frame(VMState *state, int slots, int refslots);

void vm_remove_frame(VMState *state);

void vm_error(VMState *state, const char *fmt, ...);

void vm_print_backtrace(VMState *state);

char *vm_record_backtrace(VMState *state, int *depth);

void vm_run(VMState *state);

void init_instr_fn_table();

#endif
