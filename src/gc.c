#include "gc.h"

void gc_add_roots(VMState *state, Object **objects, int num_objects, GCRootSet *set) {
  GCRootSet *prevTail = state->shared->gcstate.tail;
  state->shared->gcstate.tail = set;
  if (prevTail) prevTail->next = state->shared->gcstate.tail;
  state->shared->gcstate.tail->prev = prevTail;
  state->shared->gcstate.tail->next = NULL;
  state->shared->gcstate.tail->objects = objects;
  state->shared->gcstate.tail->num_objects = num_objects;
  return;
}

void gc_remove_roots(VMState *state, GCRootSet *entry) {
  if (entry == state->shared->gcstate.tail) {
    state->shared->gcstate.tail = entry->prev;
  }
  // cut entry out
  if (entry->prev) entry->prev->next = entry->next;
  if (entry->next) entry->next->prev = entry->prev;
}

#include <stdio.h>

// mark roots
static void gc_mark(VMState *state) {
  GCRootSet *set = state->shared->gcstate.tail;
  while (set) {
    for (int i = 0; i < set->num_objects; ++i) {
      obj_mark(state, set->objects[i]);
    }
    set = set->prev;
  }
}

// scan all allocated objects, freeing those without OBJ_GC_MARK flag
static void gc_sweep(VMState *state) {
  Object **curp = &state->shared->gcstate.last_obj_allocated;
  while (*curp) {
    int flags = (*curp)->flags;
    if (!(flags & (OBJ_GC_MARK|OBJ_IMMORTAL))) {
      Object *prev = (*curp)->prev;
      obj_free(*curp);
      state->shared->gcstate.num_obj_allocated --;
      *curp = prev; // update pointer
    } else {
      (*curp)->flags &= ~OBJ_GC_MARK; // remove flag for next run
      curp = &(*curp)->prev;
    }
  }
}

void gc_disable(VMState *state) {
  state->shared->gcstate.disabledness ++;
}

void gc_enable(VMState *state) {
  assert(state->shared->gcstate.disabledness > 0);
  state->shared->gcstate.disabledness --;
  if (state->shared->gcstate.disabledness == 0 && state->shared->gcstate.missed_gc) {
    state->shared->gcstate.missed_gc = false;
    gc_run(state); // catch up
  }
}

void gc_run(VMState *state) {
  if (state->shared->gcstate.disabledness > 0) {
    state->shared->gcstate.missed_gc = true;
    return;
  }
  gc_mark(state);
  gc_sweep(state);
}
