#include "object.h"

#include <stdio.h>
#define DEBUG_MEM 0

void gc_run(VMState *state); // defined here so we can call it in alloc

void *int_freelist = NULL;
void *table_freelist = NULL;
void *obj_freelist = NULL;

static void *cache_alloc(int size) {
  void *res = NULL;
  if (size == sizeof(IntObject)) {
    if (int_freelist) {
      res = int_freelist;
      int_freelist = *(void**) int_freelist;
    }
  }
  else if (size == sizeof(TableEntry)) {
    if (table_freelist) {
      res = table_freelist;
      table_freelist = *(void**) table_freelist;
    }
  } else if (size == sizeof(Object)) {
    if (obj_freelist) {
      res = obj_freelist;
      obj_freelist = *(void**) obj_freelist;
    }
  }
  if (!res) return calloc(size, 1);
  memset(res, 0, size);
  return res;
}

static void cache_free(int size, void *ptr) {
  switch (size) {
    case sizeof(IntObject):
      *(void**) ptr = int_freelist;
      int_freelist = ptr;
      break;
    case sizeof(TableEntry):
      *(void**) ptr = table_freelist;
      table_freelist = ptr;
      break;
    case sizeof(Object):
      *(void**) ptr = obj_freelist;
      obj_freelist = ptr;
      break;
    default:
      free(ptr);
      break;
  }
}

Object **table_lookup_ref_alloc(Table *tbl, char *key, TableEntry** first_free_ptr) {
  if (tbl->entry.name == NULL) {
    if (first_free_ptr) *first_free_ptr = &tbl->entry;
    return NULL;
  }
  TableEntry *entry = &tbl->entry, *prev_entry;
  while (entry) {
    if (key[0] == entry->name[0] && strcmp(key, entry->name) == 0) return &entry->value;
    prev_entry = entry;
    entry = entry->next;
  }
  if (first_free_ptr) {
    TableEntry *new_entry = cache_alloc(sizeof(TableEntry));
    prev_entry->next = new_entry;
    *first_free_ptr = new_entry;
  }
  return NULL;
}

Object *table_lookup(Table *tbl, char *key, bool *key_found_p) {
  Object **ptr = table_lookup_ref_alloc(tbl, key, NULL);
  if (ptr == NULL) { if (key_found_p) *key_found_p = false; return NULL; }
  if (key_found_p) *key_found_p = true;
  return *ptr;
}

Object *object_lookup(Object *obj, char *key, bool *key_found_p) {
  while (obj) {
    bool key_found;
    Object *value = table_lookup(&obj->tbl, key, &key_found);
    if (key_found) { if (key_found_p) *key_found_p = true; return value; }
    obj = obj->parent;
  }
  if (key_found_p) *key_found_p = false;
  return NULL;
}

void obj_mark(VMState *state, Object *obj) {
  if (!obj) return;
  
  if (obj->flags & OBJ_GC_MARK) return; // break cycles
  
  obj->flags |= OBJ_GC_MARK;
  
  obj_mark(state, obj->parent);
  
  TableEntry *entry = &obj->tbl.entry;
  while (entry) {
    obj_mark(state, entry->value);
    entry = entry->next;
  }
  
  bool cust_gc_found;
  Object *cust_gc_obj = object_lookup(obj, "gc", &cust_gc_found);
  if (cust_gc_found) {
    CustomGCObject *cust_gc = (CustomGCObject*) cust_gc_obj;
    cust_gc->mark_fn(state, obj);
  }
}

void obj_free(Object *obj) {
  TableEntry *entry = obj->tbl.entry.next;
  while (entry) {
    TableEntry *next = entry->next;
    cache_free(sizeof(TableEntry), entry);
    entry = next;
  }
  cache_free(obj->size, obj);
}

Object *obj_instance_of(Object *obj, Object *proto) {
  while (obj) {
    if (obj->parent == proto) return obj;
    obj = obj->parent;
  }
  return NULL;
}

Object *obj_instance_of_or_equal(Object *obj, Object *proto) {
  if (obj == proto) return obj;
  return obj_instance_of(obj, proto);
}

// change a property in-place
bool object_set_existing(Object *obj, char *key, Object *value) {
  assert(obj != NULL);
  Object *current = obj;
  while (current) {
    Object **ptr = table_lookup_ref_alloc(&current->tbl, key, NULL);
    if (ptr != NULL) {
      assert(!(current->flags & OBJ_IMMUTABLE));
      *ptr = value;
      return true;
    }
    current = current->parent;
  }
  return false;
}

// change a property but only if it exists somewhere in the prototype chain
bool object_set_shadowing(Object *obj, char *key, Object *value) {
  assert(obj != NULL);
  Object *current = obj;
  while (current) {
    Object **ptr = table_lookup_ref_alloc(&current->tbl, key, NULL);
    if (ptr) {
      // so create it in obj (not current!)
      object_set(obj, key, value);
      return true;
    }
    current = current->parent;
  }
  return false;
}

void object_set(Object *obj, char *key, Object *value) {
  assert(obj != NULL);
  TableEntry *freeptr;
  Object **ptr = table_lookup_ref_alloc(&obj->tbl, key, &freeptr);
  if (ptr) {
    assert(!(obj->flags & OBJ_IMMUTABLE));
  } else {
    assert(!(obj->flags & OBJ_CLOSED));
    freeptr->name = key;
    ptr = &freeptr->value;
  }
  *ptr = value;
}

#if OBJ_KEEP_IDS
int idcounter = 0;
#endif

static void *alloc_object_internal(VMState *state, int size) {
  if (state->num_obj_allocated > state->next_gc_run) {
    gc_run(state);
    // run gc after 20% growth or 10000 allocated or thereabouts
    state->next_gc_run = (int) (state->num_obj_allocated * 1.2) + 10000;
  }
  
  Object *res = cache_alloc(size);
  res->prev = state->last_obj_allocated;
  res->size = size;
  state->last_obj_allocated = res;
  state->num_obj_allocated ++;
  
#if OBJ_KEEP_IDS
  res->id = idcounter++;
#if DEBUG_MEM
  fprintf(stderr, "alloc object %i\n", obj->id);
#endif
#endif
  
  return res;
}

Object *alloc_object(VMState *state, Object *parent) {
  Object *obj = alloc_object_internal(state, sizeof(Object));
  obj->parent = parent;
  return obj;
}

Object *alloc_int(VMState *state, int value) {
  Object *int_base = object_lookup(state->root, "int", NULL);
  IntObject *obj = alloc_object_internal(state, sizeof(IntObject));
  obj->base.parent = int_base;
  // why though?
  // obj->base.flags |= OBJ_IMMUTABLE | OBJ_CLOSED;
  obj->value = value;
  return (Object*) obj;
}

Object *alloc_bool(VMState *state, int value) {
  Object *bool_base = object_lookup(state->root, "bool", NULL);
  BoolObject *obj = alloc_object_internal(state, sizeof(BoolObject));
  obj->base.parent = bool_base;
  // obj->base.flags |= OBJ_IMMUTABLE | OBJ_CLOSED;
  obj->value = value;
  return (Object*) obj;
}

Object *alloc_float(VMState *state, float value) {
  Object *float_base = object_lookup(state->root, "float", NULL);
  FloatObject *obj = alloc_object_internal(state, sizeof(FloatObject));
  obj->base.parent = float_base;
  // obj->base.flags |= OBJ_IMMUTABLE | OBJ_CLOSED;
  obj->value = value;
  return (Object*) obj;
}

Object *alloc_string(VMState *state, char *value) {
  Object *string_base = object_lookup(state->root, "string", NULL);
  int len = strlen(value);
  // allocate the string as part of the object, so that it gets freed with the object
  StringObject *obj = alloc_object_internal(state, sizeof(StringObject) + len + 1);
  obj->base.parent = string_base;
  // obj->base.flags |= OBJ_IMMUTABLE | OBJ_CLOSED;
  obj->value = ((char*) obj) + sizeof(StringObject);
  strncpy(obj->value, value, len + 1);
  return (Object*) obj;
}

Object *alloc_array(VMState *state, Object **ptr, int length) {
  Object *array_base = object_lookup(state->root, "array", NULL);
  ArrayObject *obj = alloc_object_internal(state, sizeof(ArrayObject));
  obj->base.parent = array_base;
  obj->ptr = ptr;
  obj->length = length;
  object_set((Object*) obj, "length", alloc_int(state, length));
  return (Object*) obj;
}

Object *alloc_ptr(VMState *state, void *ptr) { // TODO unify with alloc_fn
  Object *fn_base = object_lookup(state->root, "pointer", NULL);
  PointerObject *obj = alloc_object_internal(state, sizeof(PointerObject));
  obj->base.parent = fn_base;
  obj->ptr = ptr;
  return (Object*) obj;
}

Object *alloc_fn(VMState *state, VMFunctionPointer fn) {
  Object *fn_base = object_lookup(state->root, "function", NULL);
  FunctionObject *obj = alloc_object_internal(state, sizeof(FunctionObject));
  obj->base.parent = fn_base;
  obj->fn_ptr = fn;
  return (Object*) obj;
}

Object *alloc_custom_gc(VMState *state) {
  CustomGCObject *obj = alloc_object_internal(state, sizeof(CustomGCObject));
  obj->base.parent = NULL;
  obj->mark_fn = NULL;
  return (Object*) obj;
}
