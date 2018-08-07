#include "gc.h"

void gc_add_roots(VMState * __restrict__ state, Value *values, int num_values, GCRootSet *set) {
  GCRootSet *prevTail = state->shared->gcstate.tail.prev;
  prevTail->next = set;
  state->shared->gcstate.tail.prev = set;
  set->prev = prevTail;
  set->next = &state->shared->gcstate.tail;
  set->values = values;
  set->num_values = num_values;
  return;
}

void gc_remove_roots(VMState * __restrict__ state, GCRootSet *entry) {
  assert(entry->prev && entry->next); // the use of head/tail anchors should guarantee this
  entry->prev->next = entry->next;
  entry->next->prev = entry->prev;
}

#include <stdio.h>

// mark roots
static void gc_mark(VMState *state) {
  GCRootSet *set = state->shared->gcstate.tail.prev;
  while (set) {
    for (int i = 0; i < set->num_values; ++i) {
      Value v = set->values[i];
      if (IS_OBJ(v)) obj_mark(state, AS_OBJ(v));
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
      state->shared->gcstate.bytes_allocated -= (*curp)->size;
      obj_free(*curp);
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
  // fprintf(stderr, "run gc\n");
  // int bytes_before = state->shared->gcstate.bytes_allocated;
  gc_mark(state);
  gc_sweep(state);
  // int bytes_after = state->shared->gcstate.bytes_allocated;
  // fprintf(stderr, "done gc, %i -> %i (%f%% kept)\n", bytes_before, bytes_after, (bytes_after * 100.0) / bytes_before);
}
