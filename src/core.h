#ifndef CORE_H
#define CORE_H

#include <stdlib.h>
#include <assert.h>
#include <stdbool.h>

// core definitions (factored out because they're all super circular)

// **IMPORTANT**
// non-object values act like objects OBJ_CLOSED, OBJ_FROZEN, OBJ_NOINHERIT

struct _TableEntry;
typedef struct _TableEntry TableEntry;

struct _HashTable;
typedef struct _HashTable HashTable;

struct _VMState;
typedef struct _VMState VMState;

struct Object;
typedef struct _Object Object;

struct _HashTable {
  TableEntry *entries_ptr;
  int entries_num;
  int entries_stored;
  size_t bloom;
};

typedef enum {
  OBJ_NONE = 0,
  OBJ_CLOSED = 0x1, // no entries can be added or removed
  OBJ_FROZEN = 0x2, // no entries' values can be changed, no entries can be removed
  OBJ_NOINHERIT = 0x4, // don't allow the user to use this as a prototype
                       // used for prototypes of objects with payload,
                       // like array or function, that have their own alloc functions.
                       // you can still prototype the objects themselves though.
  OBJ_GC_MARK = 0x8,   // reachable in the "gc mark" phase
  OBJ_IMMORTAL = 0x10, // will never be freed
} ObjectFlags;

// for debugging specific objects
#define COUNT_OBJECTS 0

struct _Object {
  Object *parent;
  int size;
  ObjectFlags flags;
  Object *prev; // for gc
#if COUNT_OBJECTS
  int alloc_id;
#endif
  
  HashTable tbl;
  void (*mark_fn)(VMState *state, Object *obj); // for gc
};

typedef enum {
  TYPE_NULL, // all object references must be non-null
  TYPE_INT,
  TYPE_FLOAT,
  TYPE_BOOL,
  TYPE_OBJECT
} TypeTag;

typedef struct {
  TypeTag type;
  union {
    int i;
    float f;
    bool b;
    Object *obj;
  };
} /*__attribute__((aligned (16)))*/ Value; // TODO

typedef struct {
  Value fn;
  int this_slot;
  int args_len;
} CallInfo;

#define INFO_ARGS_PTR(I) ((int*)(I + 1))

struct _TableEntry {
  const char *name_ptr;
  size_t name_len;
  Value value;
  Object *constraint;
};

// TODO actually use
#define TBL_GRAVESTONE = ((const char*) -1);

#define IS_NULL(V) ((V).type == TYPE_NULL)

#define NOT_NULL(V) ((V).type != TYPE_NULL)

#define VNULL ((Value) { .type = TYPE_NULL, .obj = NULL })

static inline int as_int_(Value v) { assert(v.type == TYPE_INT); return v.i; }
static inline bool as_bool_(Value v) { assert(v.type == TYPE_BOOL); return v.b; }
static inline float as_float_(Value v) { assert(v.type == TYPE_FLOAT); return v.f; }
static inline Object *as_obj_(Value v) { assert(v.type == TYPE_OBJECT); return v.obj; }
static inline Object *obj_or_null_(Value v) { if (v.type == TYPE_OBJECT) return v.obj; return NULL; }

#define IS_INT(V) ((V).type == TYPE_INT)
#define IS_BOOL(V) ((V).type == TYPE_BOOL)
#define IS_FLOAT(V) ((V).type == TYPE_FLOAT)
#define IS_OBJ(V) ((V).type == TYPE_OBJECT)

#ifndef NDEBUG
#define AS_INT(V) (as_int_(V))
#define AS_BOOL(V) (as_bool_(V))
#define AS_FLOAT(V) (as_float_(V))
#define AS_OBJ(V) (as_obj_(V))
#else
#define AS_INT(V) ((V).i)
#define AS_BOOL(V) ((V).b)
#define AS_FLOAT(V) ((V).f)
#define AS_OBJ(V) ((V).obj)
#endif
#define OBJ_OR_NULL(V) (obj_or_null_(V))

#define INT2VAL(I) ((Value) { .type = TYPE_INT, .i = (I) })
#define BOOL2VAL(B) ((Value) { .type = TYPE_BOOL, .b = (B) })
#define FLOAT2VAL(F) ((Value) { .type = TYPE_FLOAT, .f = (F) })
#define OBJ2VAL(O) ((Value) { .type = TYPE_OBJECT, .obj = (O) })

#endif
