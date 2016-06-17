#define _GNU_SOURCE
#include <stdio.h>

#include "object.h"

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
  else if (size == sizeof(TablePage)) {
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
    case sizeof(TablePage):
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

Object **table_lookup_ref(TablePage *tbl, char *key) {
  // printf("::%s\n", key);
  TablePage *page = tbl;
  do {
    if (page->entries[0].name
      && key[0] == page->entries[0].name[0]
      && (!key[0] || key[1] == page->entries[0].name[1])
      && strcmp(key, page->entries[0].name) == 0
    ) return &page->entries[0].value;
    page = page->next;
  } while (page);
  return NULL;
}

Object **table_lookup_ref_alloc(TablePage *tbl, char *key, TableEntry** first_free_ptr) {
  // printf("::%s\n", key);
  TablePage *page = tbl, *prev_page;
  *first_free_ptr = NULL;
  while (page) {
    if (page->entries[0].name
      && key[0] == page->entries[0].name[0]
      && (!key[0] || key[1] == page->entries[0].name[1])
      && strcmp(key, page->entries[0].name) == 0
    ) return &page->entries[0].value;
    if (!page->entries[0].name) *first_free_ptr = &page->entries[0];
    prev_page = page;
    page = page->next;
  }
  if (!*first_free_ptr) {
    TablePage *new_page = cache_alloc(sizeof(TablePage));
    prev_page->next = new_page;
    *first_free_ptr = &new_page->entries[0];
  }
  return NULL;
}

Object *table_lookup(TablePage *tbl, char *key, bool *key_found_p) {
  Object **ptr = table_lookup_ref(tbl, key);
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
  
  TablePage *page = &obj->tbl;
  while (page) {
    obj_mark(state, page->entries[0].value);
    page = page->next;
  }
  
  Object *cust_gc_obj = NULL;
  // gc must be first entry in table
  if (obj->tbl.entries[0].name && obj->tbl.entries[0].name[0] == 'g' && obj->tbl.entries[0].name[1] == 'c' && obj->tbl.entries[0].name[2] == 0) {
    cust_gc_obj = obj->tbl.entries[0].value;
  }
  if (cust_gc_obj) {
    CustomGCObject *cust_gc = (CustomGCObject*) cust_gc_obj;
    cust_gc->mark_fn(state, obj);
  }
}

void obj_free(Object *obj) {
  TablePage *page = obj->tbl.next;
  while (page) {
    TablePage *next = page->next;
    cache_free(sizeof(TablePage), page);
    page = next;
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
// returns an error string or NULL
char *object_set_existing(Object *obj, char *key, Object *value) {
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
bool object_set_shadowing(Object *obj, char *key, Object *value) {
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
  // pin because the alloc_int may trigger gc
  GCRootSet set;
  gc_add_roots(state, (Object**) &obj, 1, &set);
  object_set((Object*) obj, "length", alloc_int(state, length));
  gc_remove_roots(state, &set);
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
