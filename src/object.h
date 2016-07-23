#ifndef OBJECT_H
#define OBJECT_H

#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdbool.h>

#include "core.h"
#include "hash.h"
#include "util.h"
#include "vm/instr.h"

struct _GCRootSet;
typedef struct _GCRootSet GCRootSet;

struct _GCRootSet {
  Value *values;
  int num_values;
  GCRootSet *prev, *next;
};

void *cache_alloc(int size);

void cache_free(int size, void *ptr);

typedef struct {
  GCRootSet *tail;
  
  Object *last_obj_allocated;
  int num_obj_allocated, next_gc_run;
#if COUNT_OBJECTS
  int num_obj_allocated_total;
#endif
  
  int disabledness;
  bool missed_gc; // tried to run gc when it was disabled
} GCState;

typedef struct _Callframe Callframe;
struct _Callframe {
  UserFunction *uf;
  Value *slots_ptr; int slots_len;
  Value **refslots_ptr; int refslots_len; // references to values in closed objects
  GCRootSet frameroot_slots; // gc entries
  Instr *instr_ptr;
  // overrides instr_ptr->belongs_to, used when in a call
  // double pointer due to Dark Magic
  FileRange **backtrace_belongs_to_p;
  Value *target_slot; // when returning to this frame, assign result value to this slot
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
  
  bool verbose;
} VMSharedState;

struct _VMState {
  VMState *parent;
  
  VMSharedState *shared;
  
  Callframe *frame;
  
  Object *root;
  Value exit_value; // set when the last stackframe returns
  
  VMRunState runstate;
  char *error;
  char *backtrace; int backtrace_depth;
};

Value *object_lookup_ref(Object *obj, const char *key);

Value *object_lookup_ref_with_hash(Object *obj, const char *key_ptr, size_t key_len, size_t hash);

Value object_lookup(Object *obj, const char *key, bool *key_found);

Value object_lookup_with_hash(Object *obj, const char *key_ptr, size_t key_len, size_t hash, bool *key_found_p);

Object *closest_obj(VMState *state, Value val);

Object *proto_obj(VMState *state, Value val);

#define OBJECT_LOOKUP_STRING(obj, key, key_found) object_lookup_with_hash(obj, key, strlen(key), hash(key, strlen(key)), key_found)

// returns NULL on success, error string otherwise
char *object_set_existing(VMState *state, Object *obj, const char *key, Value value);

char *object_set_shadowing(VMState *state, Object *obj, const char *key, Value value, bool *value_set);

char *object_set_constraint(VMState *state, Object *obj, const char *key_ptr, size_t key_len, Object *constraint);

char *object_set(VMState *state, Object *obj, const char *key, Value value);

void obj_mark(VMState *state, Object *obj);

void obj_free(Object *obj);

// returns the object in obj's prototype chain whose immediate prototype is `proto`
Object *obj_instance_of(Object *obj, Object *proto);

Object *obj_instance_of_or_equal(Object *obj, Object *proto);

bool value_instance_of(VMState *state, Value val, Object *proto);

bool value_is_truthy(Value value);

char *get_type_info(VMState*, Value);

typedef void (*VMFunctionPointer)(VMState *state, Value thisval, Value fn, Value *args_ptr, int args_len);

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
  Value *ptr;
  int length;
} ArrayObject;

// used internally
typedef struct {
  Object base;
  void *ptr;
} PointerObject;

void *alloc_object_internal(VMState *state, int size);

Value make_object(VMState *state, Object *parent);

Value make_int(VMState *state, int value);

Value make_float(VMState *state, float value);

Value make_string(VMState *state, const char *ptr, int len);

Value make_string_foreign(VMState *state, char *value);

Value make_bool(VMState *state, bool value);

Value make_array(VMState *state, Value *ptr, int length);

Value make_ptr(VMState *state, void *ptr);

Value make_fn_custom(VMState *state, VMFunctionPointer fn, int size_custom);

Value make_fn(VMState *state, VMFunctionPointer fn);

Value make_custom_gc(VMState *state);

// here so that object.c can use it
void gc_enable(VMState *state);

void gc_disable(VMState *state);

#endif
