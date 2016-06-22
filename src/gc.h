#ifndef GC_H
#define GC_H

#include "object.h"

void gc_add_roots(VMState *state, Object **objects, int num_objects, GCRootSet *set);

void gc_remove_roots(VMState *state, GCRootSet *ptr);

void gc_run(VMState *state);

void gc_init(VMState *state);

void gc_add_perm(VMState *state, Object *obj);

#endif
