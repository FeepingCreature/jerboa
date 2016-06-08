#include "gc.h"

GCState state = {0};

void *gc_add_roots(Object **objects, int num_objects) {
  RootSet *prevTail = state.tail;
  state.tail = malloc(sizeof(RootSet));
  if (prevTail) prevTail->next = state.tail;
  state.tail->prev = prevTail;
  state.tail->next = NULL;
  state.tail->objects = objects;
  state.tail->num_objects = num_objects;
  return state.tail;
}

void gc_remove_roots(void *ptr) {
  RootSet *entry = (RootSet*) ptr;
  if (entry == state.tail) {
    state.tail = entry->prev;
  }
  // cut entry out
  if (entry->prev) entry->prev->next = entry->next;
  if (entry->next) entry->next->prev = entry->prev;
  free(entry);
}

#include <stdio.h>

// mark roots
static void gc_mark(Object *context) {
  RootSet *set = state.tail;
  while (set) {
    for (int i = 0; i < set->num_objects; ++i) {
      obj_mark(context, set->objects[i]);
    }
    set = set->prev;
  }
}

// scan all allocated objects, freeing those without OBJ_GC_MARK flag
static void gc_sweep() {
  Object **curp = &last_obj_allocated;
  while (*curp) {
    if (!((*curp)->flags & OBJ_GC_MARK)) {
      Object *prev = (*curp)->prev;
      obj_free(*curp);
      num_obj_allocated --;
      *curp = prev; // update pointer
    } else {
      (*curp)->flags &= ~OBJ_GC_MARK; // remove flag for next run
      curp = &(*curp)->prev;
    }
  }
}

void gc_run(Object *context) {
  gc_mark(context);
  gc_sweep();
}
