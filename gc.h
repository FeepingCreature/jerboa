#ifndef GC_H
#define GC_H

#include "object.h"

struct _RootSet;
typedef struct _RootSet RootSet;

typedef struct _RootSet {
  Object **objects;
  int num_objects;
  RootSet *prev, *next;
} RootSet;

typedef struct {
  RootSet *tail;
} GCState;

GCState state;

void *gc_add_roots(Object **objects, int num_objects);

void gc_remove_roots(void *ptr);

void gc_run();

#endif
