#ifndef GC_H
#define GC_H

#include "object.h"

void gc_add_roots(VMState *state, Value *values, int num_values, GCRootSet *set);

void gc_remove_roots(VMState *state, GCRootSet *ptr);

void gc_run(VMState *state);

#endif
