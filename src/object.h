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

void *cache_alloc(int size);

void cache_free(int size, void *ptr);

void save_profile_output(char *file, TextRange source, VMProfileState *profile_state);

Value *object_lookup_ref(Object *obj, const char *key);

TableEntry *object_lookup_ref_with_hash(Object *obj, const char *key_ptr, size_t key_len, size_t hashv);

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

bool value_fits_constraint(VMSharedState *sstate, Value value, Object *constraint);

char *get_type_info(VMState*, Value);

char *get_val_info(Value val);

static inline Value load_arg(Callframe *frame, Arg arg) {
  if (arg.kind == ARG_SLOT) {
    assert(arg.slot < frame->slots_len);
    return frame->slots_ptr[arg.slot];
  }
  if (arg.kind == ARG_REFSLOT) {
    assert(arg.refslot < frame->refslots_len);
    return frame->refslots_ptr[arg.refslot]->value;
  }
  assert(arg.kind == ARG_VALUE);
  return arg.value;
}

static inline RefArg ref_arg(Callframe *frame, WriteArg warg) {
  if (warg.kind == ARG_REFSLOT) {
    assert(warg.refslot < frame->refslots_len);
    return (RefArg) { .kind = ARG_REFSLOT, .refslot_p = frame->refslots_ptr[warg.refslot] };
  }
  assert(warg.kind == ARG_SLOT);
  return (RefArg) { .kind = ARG_SLOT, .slot_p = &frame->slots_ptr[warg.refslot] };
}

static inline char *set_ref(VMState *state, RefArg arg, Value value) {
  if (arg.kind == ARG_REFSLOT) {
    Object *constraint = arg.refslot_p->constraint;
    if (UNLIKELY(constraint && !value_fits_constraint(state->shared, value, constraint))) {
      return my_asprintf("value failed type constraint: constraint was %s, but value was %s",
                        get_type_info(state, OBJ2VAL(constraint)), get_type_info(state, value));
    }
    arg.refslot_p->value = value;
    return NULL;
  }
  assert(arg.kind == ARG_SLOT);
  *arg.slot_p = value;
  return NULL;
}

static inline void set_arg(VMState *state, WriteArg warg, Value value) {
  char *error = set_ref(state, ref_arg(state->frame, warg), value);
  VM_ASSERT(!error, error);
}

static inline void vm_return(VMState *state, Value val) {
  char *error = set_ref(state, state->frame->target, val);
  VM_ASSERT(!error, error);
}

// args_ptr's entries are guaranteed to lie inside slots_ptr.
typedef void (*VMFunctionPointer)(VMState *state, CallInfo *info);

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
