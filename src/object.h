#ifndef OBJECT_H
#define OBJECT_H

#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdbool.h>

#include "hash.h"
#include "util.h"
#include "vm/instr.h"

typedef enum {
  OBJ_NONE = 0,
  OBJ_CLOSED = 0x1, // no entries can be added or removed
  OBJ_FROZEN = 0x2, // no entries' values can be changed, no entries can be removed
  OBJ_NOINHERIT = 0x4, // don't allow the user to use this as a prototype
                       // used for prototypes of objects with payload,
                       // like int or float, that have their own alloc functions.
                       // you can still prototype the objects themselves though.
  OBJ_GC_MARK = 0x8,   // reachable in the "gc mark" phase
  OBJ_PRIMITIVE = 0x10,// no table, no mark_fn
  OBJ_PRIMITIVE_VALUE = OBJ_FROZEN | OBJ_CLOSED | OBJ_PRIMITIVE | OBJ_NOINHERIT // primitive must entail noinherit!
} ObjectFlagsBase;

typedef short int ObjectFlags;

struct _VMState;
typedef struct _VMState VMState;

struct _Object {
  Object *parent;
  short int size;
  ObjectFlags flags;
  Object *prev; // for gc
  
  union {
    struct {
      HashTable tbl;
      void (*mark_fn)(VMState *state, Object *obj); // for gc
    };
    int int_value;
    float float_value;
    bool bool_value;
    struct {
      union { int _int; float _float; bool _bool; };
      int _primitive_end_marker;
    };
  };
};

#define PRIMITIVE_OBJ_SIZE offsetof(Object, _primitive_end_marker)

struct _GCRootSet;
typedef struct _GCRootSet GCRootSet;

struct _GCRootSet {
  Object **objects;
  int num_objects;
  GCRootSet *prev, *next;
};

void *cache_alloc(int size);

void cache_free(int size, void *ptr);

typedef struct {
  GCRootSet *tail;
  
  Object *last_obj_allocated;
  int num_obj_allocated, next_gc_run;
  
  GCRootSet permanents; // objects that never get freed, globals, instr-cached objects, etc.
  int disabledness;
  bool missed_gc; // tried to run gc when it was disabled
} GCState;

typedef struct _Callframe Callframe;
struct _Callframe {
  UserFunction *uf;
  Object **slots_ptr; int slots_len;
  Object ***refslots_ptr; int refslots_len; // references to values in closed objects
  GCRootSet frameroot_slots; // gc entries
  Instr *instr_ptr;
  int block, prev_block; // required for phi nodes
  Callframe *above;
};

typedef enum {
  VM_TERMINATED,
  VM_RUNNING,
  VM_ERRORED
} VMRunState;

typedef struct {
  struct timespec last_prof_time;
  
  HashTable direct_table;
  HashTable indirect_table;
} VMProfileState;

void save_profile_output(char *file, TextRange source, VMProfileState *profile_state);

typedef struct {
  Object *bool_false, *bool_true;
  Object *int_zero; // TODO array
  Object *int_base, *bool_base, *float_base;
  Object *closure_base, *function_base;
  Object *array_base, *string_base, *pointer_base;
  Object *ffi_obj; // cached here so ffi_call_fn can be fast
} ValueCache;

// shared between parent and child VMs
typedef struct {
  GCState gcstate;
  VMProfileState profstate;
  ValueCache vcache;
  int cyclecount;
  
  // backing storage for stack allocations
  // cannot be moved after the fact!
  // stored in shared_state because sub-vms stick to callstack order
  void *stack_data_ptr; int stack_data_len;
  int stack_data_offset;
} VMSharedState;

struct _VMState {
  VMState *parent;
  
  VMSharedState *shared;
  
  Callframe *frame;
  
  Object *root;
  Object *result_value;
  
  VMRunState runstate;
  char *error;
  char *backtrace; int backtrace_depth;
};

Object **object_lookup_ref(Object *obj, const char *key);

Object **object_lookup_ref_with_hash(Object *obj, const char *key_ptr, size_t key_len, size_t hash);

Object *object_lookup(Object *obj, const char *key, bool *key_found);

Object *object_lookup_with_hash(Object *obj, const char *key_ptr, size_t key_len, size_t hash, bool *key_found);

#define OBJECT_LOOKUP_STRING(obj, key, key_found) object_lookup_with_hash(obj, key, strlen(key), hash(key, strlen(key)), key_found)

// returns NULL on success, error string otherwise
char *object_set_existing(Object *obj, const char *key, Object *value);

char *object_set_shadowing(Object *obj, const char *key, Object *value, bool *value_set);

char *object_set_constraint(VMState *state, Object *obj, const char *key_ptr, size_t key_len, Object *constraint);

char *object_set(Object *obj, const char *key, Object *value);

void obj_mark(VMState *state, Object *obj);

void obj_free(Object *obj);

// returns the object in obj's prototype chain whose immediate prototype is `proto`
Object *obj_instance_of(Object *obj, Object *proto);

Object *obj_instance_of_or_equal(Object *obj, Object *proto);

bool obj_is_truthy(VMState *state, Object *obj);

char *get_type_info(VMState*, Object*);

typedef void (*VMFunctionPointer)(VMState *state, Object *thisptr, Object *fn, Object **args_ptr, int args_len);

typedef struct {
  Object base;
  VMFunctionPointer fn_ptr;
} FunctionObject;

typedef struct {
  Object base;
  VMFunctionPointer fn_ptr;
  Object *context;
  UserFunction *vmfun;
  int num_called; // used for triggering optimization
} ClosureObject;

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

void *alloc_object_internal(VMState *state, int size);

Object *alloc_object(VMState *state, Object *parent);

Object *alloc_int(VMState *state, int value);

Object *alloc_float(VMState *state, float value);

Object *alloc_string(VMState *state, const char *ptr, int len);

Object *alloc_string_foreign(VMState *state, char *value);

// returns bool object from value cache
Object *alloc_bool(VMState *state, bool value);

// returns newly allocated bool object (used to initialize value_cache)
Object *alloc_bool_uncached(VMState *state, bool value);

Object *alloc_array(VMState *state, Object **ptr, Object *length);

Object *alloc_ptr(VMState *state, void *ptr); // TODO unify with alloc_fn

Object *alloc_fn_custom(VMState *state, VMFunctionPointer fn, int size_custom);

Object *alloc_fn(VMState *state, VMFunctionPointer fn);

Object *alloc_custom_gc(VMState *state);

// here so that object.c can use it
void gc_enable(VMState *state);

void gc_disable(VMState *state);

#endif
