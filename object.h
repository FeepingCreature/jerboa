#ifndef OBJECT_H
#define OBJECT_H

#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdbool.h>

#include "vm/instr.h"

typedef enum {
  OBJ_NONE = 0,
  OBJ_CLOSED = 0x1, // no entries can be added or removed
  OBJ_IMMUTABLE = 0x2, // no entries' values can be changed
  OBJ_NOINHERIT = 0x4, // don't allow the user to use this as a prototype
                       // used for prototypes of objects with payload,
                       // like int or float, that have their own alloc functions.
                       // you can still prototype the objects themselves though.
  OBJ_GC_MARK = 0x8   // reachable in the "gc mark" phase
} ObjectFlags;

struct _Object;
typedef struct _Object Object;

struct _TableEntry;
typedef struct _TableEntry TableEntry;

struct _Table;
typedef struct _Table Table;

struct _TableEntry {
  char *name;
  Object *value;
  TableEntry *next;
};

struct _Table {
  TableEntry entry;
};

#define OBJ_KEEP_IDS 0

struct _Object {
  Table tbl;
  Object *parent;
  int size;
  ObjectFlags flags;
#if OBJ_KEEP_IDS
  int id;
#endif
  Object *prev; // for gc
};

struct _GCRootSet;
typedef struct _GCRootSet GCRootSet;

struct _GCRootSet {
  Object **objects;
  int num_objects;
  GCRootSet *prev, *next;
};

typedef struct {
  GCRootSet *tail;
} GCState;

typedef struct {
  UserFunction *uf;
  Object *context;
  Object **slots_ptr; int slots_len;
  GCRootSet frameroot; // gc entry for the pinned slots array
  InstrBlock *block;
  int instr_offs;
} Callframe;

typedef struct {
  Callframe *stack_ptr; int stack_len;
  Object *root;
  Object *result_value;
  
  // memory handling
  GCState gcstate;
  Object *last_obj_allocated;
  int num_obj_allocated, next_gc_run;
} VMState;

Object *object_lookup(Object *obj, char *key, bool *key_found);

void object_set_existing(Object *obj, char *key, Object *value);

void object_set_shadowing(Object *obj, char *key, Object *value);

void object_set(Object *obj, char *key, Object *value);

void obj_mark(VMState *state, Object *obj);

void obj_free(Object *obj);

// returns the object in obj's prototype chain whose immediate prototype is `proto`
Object *obj_instance_of(Object *obj, Object *proto);

Object *obj_instance_of_or_equal(Object *obj, Object *proto);

typedef void (*VMFunctionPointer)(VMState *state, Object *thisptr, Object *fn, Object **args_ptr, int args_len);

typedef struct {
  Object base;
  VMFunctionPointer fn_ptr;
} FunctionObject;

typedef struct {
  FunctionObject base;
  Object *context;
  UserFunction vmfun;
} ClosureObject;

typedef struct {
  Object base;
  int value;
} IntObject;

typedef struct {
  Object base;
  bool value;
} BoolObject;

typedef struct {
  Object base;
  float value;
} FloatObject;

typedef struct {
  Object base;
  char *value;
} StringObject;

typedef struct {
  Object base;
  Object **ptr;
  int length;
} ArrayObject;

// used internally
typedef struct {
  Object base;
  void *ptr;
} PointerObject;

typedef struct {
  Object base;
  void (*mark_fn)(VMState *state, Object *obj);
} CustomGCObject;

Object *alloc_object(VMState *state, Object *parent);

Object *alloc_int(VMState *state, int value);

Object *alloc_float(VMState *state, float value);

Object *alloc_string(VMState *state, char *value);

Object *alloc_bool(VMState *state, int value);

Object *alloc_array(VMState *state, Object **ptr, int length);

Object *alloc_ptr(VMState *state, void *ptr); // TODO unify with alloc_fn

Object *alloc_fn(VMState *state, VMFunctionPointer fn);

Object *alloc_custom_gc(VMState *state);

#endif
