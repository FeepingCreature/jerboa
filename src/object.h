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
#include "vm/vm.h"
#include "vm/instr.h"

void *cache_alloc_uninitialized(int size);

static inline void *cache_alloc(int size) {
  void *res = cache_alloc_uninitialized(size);
  bzero(res, size);
  return res;
}

void cache_free(int size, void *ptr);

void free_cache(VMState *state);

void save_profile_output(char *file, VMProfileState *profile_state);

Value object_lookup(Object *obj, FastKey *key, bool *key_found);

Object *closest_obj(VMState *state, Value val);

Object *proto_obj(VMState *state, Value val);

// hope the compiler will inline prepare_key
static inline Value object_lookup_key_internal(Object *obj, FastKey key, bool *key_found) {
  return object_lookup(obj, &key, key_found);
}
#define OBJECT_LOOKUP_STRING(obj, key, key_found) object_lookup_key_internal(obj, prepare_key(key, strlen(key)), key_found)

// returns NULL on success, error string otherwise
char *object_set_existing(VMState *state, Object *obj, FastKey *key, Value value);

char *object_set_shadowing(VMState *state, Object *obj, FastKey *key, Value value, bool *value_set);

char *object_set_constraint(VMState *state, Object *obj, FastKey *key, Object *constraint);

char *object_set(VMState *state, Object *obj, FastKey *key, Value value);

static inline char *object_set_key_internal(VMState *state, Object *obj, FastKey key, Value value) {
  return object_set(state, obj, &key, value);
}
#define OBJECT_SET_STRING(state, obj, key, value) object_set_key_internal(state, obj, prepare_key(key, strlen(key)), value)

void obj_mark(VMState *state, Object *obj);

void obj_free_aux(Object *obj); // free everything attached to obj except obj

void obj_free(Object *obj);

// returns the object in obj's prototype chain whose immediate prototype is `proto`
Object *obj_instance_of(Object *obj, Object *proto);

bool value_instance_of(VMState *state, Value val, Object *proto);

bool value_is_truthy(Value value);

bool value_fits_constraint(VMSharedState *sstate, Value value, Object *constraint);

char *get_type_info(VMState*, Value);

char *get_val_info(VMState *state, Value val);

static inline Value load_arg(Callframe *frame, Arg arg) {
  if (arg.kind == ARG_SLOT) {
    assert(arg.slot < frame->slots_len);
  } else if (arg.kind == ARG_REFSLOT) {
    assert(arg.refslot < frame->refslots_len);
  } else assert(arg.kind == ARG_VALUE);
  
  if (arg.kind == ARG_SLOT) {
    return frame->slots_ptr[arg.slot];
  }
  if (arg.kind == ARG_REFSLOT) {
    return frame->refslots_ptr[arg.refslot]->value;
  }
  return arg.value;
  // NOT faster
  /*
  Value *ptrs[] = {
    &frame->slots_ptr[arg.slot],
    &frame->refslots_ptr[arg.refslot]->value,
    &arg.value
  };
  return *ptrs[arg.kind];
  */
}

static inline void set_arg(VMState *state, WriteArg warg, Value value) {
  if (warg.kind == ARG_REFSLOT) {
    assert(warg.refslot < state->frame->refslots_len);
    TableEntry *entry = state->frame->refslots_ptr[warg.refslot];
    Object *constraint = entry->constraint;
    VM_ASSERT(!constraint || value_fits_constraint(state->shared, value, constraint),
              "value failed type constraint: constraint was %s, but value was %s",
              get_type_info(state, OBJ2VAL(constraint)), get_type_info(state, value));
    entry->value = value;
    return;
  }
  if (UNLIKELY(warg.kind == ARG_POINTER)) {
    *warg.pointer = value;
    return;
  }
  assert(warg.kind == ARG_SLOT);
  state->frame->slots_ptr[warg.slot] = value;
}

static inline void vm_return(VMState *state, CallInfo *info, Value val) {
  set_arg(state, info->target, val);
}

// such as intrinsics
typedef struct {
  Object base;
  VMFunctionPointer fn_ptr;
} FunctionObject;

// such as script functions
typedef struct {
  Object base;
  Object *context;
  UserFunction *vmfun;
  int num_called; // used for triggering optimization
} ClosureObject;

typedef struct {
  Object base;
  bool static_ptr;
  char *value;
} StringObject;

typedef struct {
  Object base;
  Value *ptr;
  bool owned;
  int length;
  int capacity;
} ArrayObject;

// used internally
typedef struct {
  Object base;
  void *ptr;
} PointerObject;

void *alloc_object_internal(VMState *state, int size, bool stack);

Value make_object(VMState *state, Object *parent, bool stack);

Value make_string(VMState *state, const char *ptr, int len);

Value make_string_static(VMState *state, char *value);

Value make_array(VMState *state, Value *ptr, int length, bool owned);

void array_resize(VMState *state, ArrayObject *aobj, int newsize, bool update_len);

Value make_ptr(VMState *state, void *ptr);

Value make_fn_custom(VMState *state, VMFunctionPointer fn, int size_custom);

Value make_fn(VMState *state, VMFunctionPointer fn);

Value make_custom_gc(VMState *state);

// here so that object.c can use it
void gc_enable(VMState *state);

void gc_disable(VMState *state);

#endif
