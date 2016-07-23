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

void *cache_alloc(int size);

void cache_free(int size, void *ptr);

void save_profile_output(char *file, TextRange source, VMProfileState *profile_state);

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

// args_ptr's entries are guaranteed to lie inside slots_ptr.
typedef void (*VMFunctionPointer)(VMState *state, CallInfo *info);

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

char *get_val_info(Value val);

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
