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
  OBJ_IMMUTABLE = 0x2 // no entries' values can be changed
} ObjectFlags;

struct _Object;
typedef struct _Object Object;

struct _TableEntry;
typedef struct _TableEntry TableEntry;

struct _Table;
typedef struct _Table Table;

typedef struct _TableEntry {
  char *name;
  Object *value;
  TableEntry *next;
} TableEntry;

typedef struct _Table {
  TableEntry entry;
} Table;

#define OBJ_KEEP_IDS 0

typedef struct _Object {
  Object *parent;
  ObjectFlags flags;
#if OBJ_KEEP_IDS
  int id;
#endif
  int refs;
  Table tbl;
} Object;

void obj_claim(Object*);

Object *obj_claimed(Object*);

void obj_free(Object*);

Object *object_lookup(Object *obj, char *key);

void object_set_existing(Object *obj, char *key, Object *value);

void object_set(Object *obj, char *key, Object *value);

typedef Object* (*VMFunctionPointer)(Object *context, Object *fn, Object **args_ptr, int args_len);

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

Object *alloc_object(Object *parent);

Object *alloc_int(Object *context, int value);

Object *alloc_bool(Object *context, int value);

Object *alloc_fn(Object *context, VMFunctionPointer fn);

#endif
