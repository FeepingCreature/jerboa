#include "gc.h"

void gc_add_roots(VMState *state, Object **objects, int num_objects, GCRootSet *set) {
  GCRootSet *prevTail = state->gcstate.tail;
  state->gcstate.tail = set;
  if (prevTail) prevTail->next = state->gcstate.tail;
  state->gcstate.tail->prev = prevTail;
  state->gcstate.tail->next = NULL;
  state->gcstate.tail->objects = objects;
  state->gcstate.tail->num_objects = num_objects;
  return;
}

void gc_remove_roots(VMState *state, GCRootSet *entry) {
  if (entry == state->gcstate.tail) {
    state->gcstate.tail = entry->prev;
  }
  // cut entry out
  if (entry->prev) entry->prev->next = entry->next;
  if (entry->next) entry->next->prev = entry->prev;
}

#include <stdio.h>

// mark roots
static void gc_mark(VMState *state) {
  GCRootSet *set = state->gcstate.tail;
  while (set) {
    for (int i = 0; i < set->num_objects; ++i) {
      obj_mark(state, set->objects[i]);
    }
    set = set->prev;
  }
}

// scan all allocated objects, freeing those without OBJ_GC_MARK flag
static void gc_sweep(VMState *state) {
  Object **curp = &state->last_obj_allocated;
  while (*curp) {
    if (!((*curp)->flags & OBJ_GC_MARK)) {
      Object *prev = (*curp)->prev;
      obj_free(*curp);
      state->num_obj_allocated --;
      *curp = prev; // update pointer
    } else {
      (*curp)->flags &= ~OBJ_GC_MARK; // remove flag for next run
      curp = &(*curp)->prev;
    }
  }
}

void gc_disable(VMState *state) {
  state->gcstate.disabledness ++;
}

void gc_enable(VMState *state) {
  assert(state->gcstate.disabledness > 0);
  state->gcstate.disabledness --;
}

void gc_run(VMState *state) {
  if (state->gcstate.disabledness > 0) return;
  gc_mark(state);
  gc_sweep(state);
}
