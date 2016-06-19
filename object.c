#define _GNU_SOURCE
#include <stdio.h>

#include "object.h"

#define DEBUG_MEM 0

void gc_run(VMState *state); // defined here so we can call it in alloc

void *int_freelist = NULL;
void *obj_freelist = NULL;
void *table4_freelist = NULL, *table8_freelist = NULL, *table16_freelist = NULL;

static void *cache_alloc(int size) {
  // return calloc(size, 1);
  void *res = NULL;
  switch (size) {
    case sizeof(IntObject):
      if (int_freelist) {
        res = int_freelist;
        int_freelist = *(void**) int_freelist;
      }
      break;
    case sizeof(TableEntry) * 4:
      if (table4_freelist) {
        res = table4_freelist;
        table4_freelist = *(void**) table4_freelist;
      }
      break;
    case sizeof(TableEntry) * 8:
      if (table8_freelist) {
        res = table8_freelist;
        table8_freelist = *(void**) table8_freelist;
      }
      break;
    case sizeof(TableEntry) * 16:
      if (table16_freelist) {
        res = table16_freelist;
        table16_freelist = *(void**) table16_freelist;
      }
      break;
    case sizeof(Object):
      if (obj_freelist) {
        res = obj_freelist;
        obj_freelist = *(void**) obj_freelist;
      }
      break;
    default: break;
  }
  if (!res) return calloc(size, 1);
  memset(res, 0, size);
  return res;
}

static void cache_free(int size, void *ptr) {
  // free(ptr); return;
  switch (size) {
    case sizeof(IntObject):
      *(void**) ptr = int_freelist;
      int_freelist = ptr;
      break;
    case sizeof(TableEntry) * 4:
      *(void**) ptr = table4_freelist;
      table4_freelist = ptr;
      break;
    case sizeof(TableEntry) * 8:
      *(void**) ptr = table8_freelist;
      table8_freelist = ptr;
      break;
    case sizeof(TableEntry) * 16:
      *(void**) ptr = table16_freelist;
      table16_freelist = ptr;
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

// thanks http://stackoverflow.com/questions/7666509/hash-function-for-string
static unsigned long djb2(const char *str) {
  unsigned long hash = 5381;
  int c;
  while ((c = *str++) != 0) hash = ((hash << 5) + hash) + c; /* hash * 33 + c */
  return hash;
} 

Object **table_lookup_ref(HashTable *tbl, const char *key) {
  assert(key != NULL);
  if (tbl->entries_stored == 0) return NULL;
  // printf(":: %s into %i having %i\n", key, tbl->entries_num, tbl->entries_stored);
  int entries_num = tbl->entries_num;
  if (entries_num <= 4) { // faster to do a direct scan
    for (int i = 0; i < entries_num; ++i) {
      TableEntry *entry = &tbl->entries_ptr[i];
      if (entry->name
        && key[0] == entry->name[0]
        && (!key[0] || key[1] == entry->name[1])
        && strcmp(key, entry->name) == 0
      ) return &entry->value;
    }
  } else {
    int entries_mask = entries_num - 1;
    int base = djb2(key);
    for (int i = 0; i < entries_num; ++i) {
      int k = (base + i) & entries_mask;
      TableEntry *entry = &tbl->entries_ptr[k];
      if (entry->name == NULL) return NULL;
      if (key[0] == entry->name[0]
        && (!key[0] || key[1] == entry->name[1])
        && strcmp(key, entry->name) == 0
      ) return &entry->value;
    }
  }
  // scanned the complete table; this really should not have happened
  return NULL;
}

// if the key was not found, return null but allocate a mapping in first_free_ptr
Object **table_lookup_ref_alloc(HashTable *tbl, const char *key, Object*** first_free_ptr) {
  assert(key != NULL);
  *first_free_ptr = NULL;
  // printf(":: %s into %i having %i\n", key, tbl->entries_num, tbl->entries_stored);
  int entries_num = tbl->entries_num;
  int entries_mask = entries_num - 1;
  int base = entries_num?djb2(key):0;
  TableEntry *free_ptr;
  for (int i = 0; i < entries_num; ++i) {
    int k = (base + i) & entries_mask;
    TableEntry *entry = &tbl->entries_ptr[k];
    if (entry->name
      && key[0] == entry->name[0]
      && (!key[0] || key[1] == entry->name[1])
      && strcmp(key, entry->name) == 0
    ) return &entry->value;
    if (entry->name == NULL) {
      free_ptr = entry;
      break;
    }
  }
  int newlen;
  if (entries_num == 0) {
    // definitely resize
    newlen = 4;
  } else {
    int fillrate = (tbl->entries_stored * 100) / entries_num;
    if (fillrate < 70) {
      // printf("--%p:   fillrate is okay with %i, set %li to %s\n", (void*) tbl, fillrate, free_ptr - tbl->entries_ptr, key);
      assert(free_ptr); // should have been found above
      free_ptr->name = key;
      tbl->entries_stored ++;
      *first_free_ptr = &free_ptr->value;
      return NULL;
    }
    newlen = entries_num * 2;
  }
  HashTable newtable;
  newtable.entries_ptr = cache_alloc(sizeof(TableEntry) * newlen);
  newtable.entries_num = newlen;
  newtable.entries_stored = 0;
  for (int i = 0; i < entries_num; ++i) {
    TableEntry *entry = &tbl->entries_ptr[i];
    if (entry->name) {
      Object **freeptr;
      Object **nope = table_lookup_ref_alloc(&newtable, entry->name, &freeptr);
      assert(nope == NULL); // double entry??
      *freeptr = entry->value;
    }
  }
  cache_free(sizeof(TableEntry) * tbl->entries_num, tbl->entries_ptr);
  *tbl = newtable;
  // and redo with new size!
  return table_lookup_ref_alloc(tbl, key, first_free_ptr);
}

Object *table_lookup(HashTable *tbl, const char *key, bool *key_found_p) {
  Object **ptr = table_lookup_ref(tbl, key);
  if (ptr == NULL) { if (key_found_p) *key_found_p = false; return NULL; }
  if (key_found_p) *key_found_p = true;
  return *ptr;
}

Object *object_lookup(Object *obj, const char *key, bool *key_found_p) {
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
  
  HashTable *tbl = &obj->tbl;
  for (int i = 0; i < tbl->entries_num; ++i) {
    TableEntry *entry = &tbl->entries_ptr[i];
    if (entry->name) {
      obj_mark(state, entry->value);
    }
  }
  
  Object *current = obj;
  while (current) {
    if (current->mark_fn) {
      current->mark_fn(state, obj);
    }
    current = current->parent;
  }
}

void obj_free(Object *obj) {
  cache_free(sizeof(TableEntry) * obj->tbl.entries_num, obj->tbl.entries_ptr);
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
// returns an error string or NULL
char *object_set_existing(Object *obj, const char *key, Object *value) {
  assert(obj != NULL);
  Object *current = obj;
  while (current) {
    Object **ptr = table_lookup_ref(&current->tbl, key);
    if (ptr != NULL) {
      if (current->flags & OBJ_IMMUTABLE) {
        char *error = NULL;
        if (-1 == asprintf(&error, "Tried to set existing key '%s', but object %p was immutable.", key, (void*) current)) abort();
        return error;
      }
      assert(!(current->flags & OBJ_IMMUTABLE));
      *ptr = value;
      return NULL;
    }
    current = current->parent;
  }
  char *error = NULL;
  if (-1 == asprintf(&error, "Key '%s' not found in object %p.", key, (void*) obj)) abort();
  return error;
}

// change a property but only if it exists somewhere in the prototype chain
bool object_set_shadowing(Object *obj, const char *key, Object *value) {
  assert(obj != NULL);
  Object *current = obj;
  while (current) {
    Object **ptr = table_lookup_ref(&current->tbl, key);
    if (ptr) {
      // so create it in obj (not current!)
      object_set(obj, key, value);
      return true;
    }
    current = current->parent;
  }
  return false;
}

void object_set(Object *obj, const char *key, Object *value) {
  assert(obj != NULL);
  Object **freeptr;
  Object **ptr = table_lookup_ref_alloc(&obj->tbl, key, &freeptr);
  if (ptr) {
    assert(!(obj->flags & OBJ_IMMUTABLE));
  } else {
    assert(!(obj->flags & OBJ_CLOSED));
    ptr = freeptr;
  }
  *ptr = value;
}

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
  
#if DEBUG_MEM
  fprintf(stderr, "alloc object %p\n", (void*) obj);
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
  assert(int_base);
  IntObject *obj = alloc_object_internal(state, sizeof(IntObject));
  obj->base.parent = int_base;
  // why though?
  // obj->base.flags |= OBJ_IMMUTABLE | OBJ_CLOSED;
  obj->value = value;
  return (Object*) obj;
}

Object *alloc_bool(VMState *state, int value) {
  Object *bool_base = object_lookup(state->root, "bool", NULL);
  assert(bool_base);
  BoolObject *obj = alloc_object_internal(state, sizeof(BoolObject));
  obj->base.parent = bool_base;
  // obj->base.flags |= OBJ_IMMUTABLE | OBJ_CLOSED;
  obj->value = value;
  return (Object*) obj;
}

Object *alloc_float(VMState *state, float value) {
  Object *float_base = object_lookup(state->root, "float", NULL);
  assert(float_base);
  FloatObject *obj = alloc_object_internal(state, sizeof(FloatObject));
  obj->base.parent = float_base;
  // obj->base.flags |= OBJ_IMMUTABLE | OBJ_CLOSED;
  obj->value = value;
  return (Object*) obj;
}

Object *alloc_string(VMState *state, const char *value) {
  Object *string_base = object_lookup(state->root, "string", NULL);
  assert(string_base);
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
  assert(array_base);
  ArrayObject *obj = alloc_object_internal(state, sizeof(ArrayObject));
  obj->base.parent = array_base;
  obj->ptr = ptr;
  obj->length = length;
  gc_disable(state);
  object_set((Object*) obj, "length", alloc_int(state, length));
  gc_enable(state);
  return (Object*) obj;
}

Object *alloc_ptr(VMState *state, void *ptr) { // TODO unify with alloc_fn
  Object *ptr_base = object_lookup(state->root, "pointer", NULL);
  assert(ptr_base);
  PointerObject *obj = alloc_object_internal(state, sizeof(PointerObject));
  obj->base.parent = ptr_base;
  obj->ptr = ptr;
  return (Object*) obj;
}

Object *alloc_fn(VMState *state, VMFunctionPointer fn) {
  Object *fn_base = object_lookup(state->root, "function", NULL);
  assert(fn_base);
  FunctionObject *obj = alloc_object_internal(state, sizeof(FunctionObject));
  obj->base.parent = fn_base;
  obj->fn_ptr = fn;
  return (Object*) obj;
}
