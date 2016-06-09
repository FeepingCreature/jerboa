#include "object.h"

#include <stdio.h>
#define DEBUG_MEM 0

void gc_run(Object *context); // defined here so we can call it in alloc

Object *last_obj_allocated = NULL;
int num_obj_allocated = 0;
int next_gc_run = 10000;

Object **table_lookup_ref_alloc(Table *tbl, char *key, TableEntry** first_free_ptr) {
  if (tbl->entry.name == NULL) {
    if (first_free_ptr) *first_free_ptr = &tbl->entry;
    return NULL;
  }
  TableEntry *entry = &tbl->entry, *prev_entry;
  while (entry) {
    if (strcmp(key, entry->name) == 0) return &entry->value;
    prev_entry = entry;
    entry = entry->next;
  }
  if (first_free_ptr) {
    TableEntry *new_entry = calloc(sizeof(TableEntry), 1);
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

void obj_mark(Object *context, Object *obj) {
  if (!obj) return;
  
  if (obj->flags & OBJ_GC_MARK) return; // break cycles
  
  obj->flags |= OBJ_GC_MARK;
  
  obj_mark(context, obj->parent);
  
  TableEntry *entry = &obj->tbl.entry;
  while (entry) {
    obj_mark(context, entry->value);
    entry = entry->next;
  }
  
  bool mark_fn_found;
  Object *mark_fn = object_lookup(obj, "gc_mark", &mark_fn_found);
  if (mark_fn_found) {
    FunctionObject *mark_fn_fnobj = (FunctionObject*) mark_fn;
    mark_fn_fnobj->fn_ptr(context, obj, mark_fn, NULL, 0);
  }
}

void obj_free(Object *obj) {
  TableEntry *entry = obj->tbl.entry.next;
  while (entry) {
    TableEntry *next = entry->next;
    free(entry);
    entry = next;
  }
  free(obj);
}

Object *obj_instance_of(Object *obj, Object *proto) {
  while (obj) {
    if (obj->parent == proto) return obj;
    obj = obj->parent;
  }
  return NULL;
}

// change a property in-place
void object_set_existing(Object *obj, char *key, Object *value) {
  assert(obj != NULL);
  Object *current = obj;
  while (current) {
    Object **ptr = table_lookup_ref_alloc(&current->tbl, key, NULL);
    if (ptr != NULL) {
      assert(!(current->flags & OBJ_IMMUTABLE));
      *ptr = value;
      return;
    }
    current = current->parent;
  }
  fprintf(stderr, "key '%s' not found in object\n", key);
  assert(false);
}

// change a property but only if it exists somewhere in the prototype chain
void object_set_shadowing(Object *obj, char *key, Object *value) {
  assert(obj != NULL);
  Object *current = obj;
  while (current) {
    Object **ptr = table_lookup_ref_alloc(&current->tbl, key, NULL);
    if (ptr) {
      // so create it in obj (not current!)
      object_set(obj, key, value);
      return;
    }
    current = current->parent;
  }
  fprintf(stderr, "key '%s' not found in object\n", key);
  assert(false);
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

static void *alloc_object_internal(Object *context, int size) {
  if (num_obj_allocated > next_gc_run) {
    gc_run(context);
    next_gc_run = (int) (num_obj_allocated * 1.2); // run gc after 20% growth
  }
  
  Object *res = calloc(size, 1);
  res->prev = last_obj_allocated;
  last_obj_allocated = res;
  num_obj_allocated ++;
  
#if OBJ_KEEP_IDS
  res->id = idcounter++;
#if DEBUG_MEM
  fprintf(stderr, "alloc object %i\n", obj->id);
#endif
#endif
  
  return res;
}

Object *alloc_object(Object *context, Object *parent) {
  Object *obj = alloc_object_internal(context, sizeof(Object));
  obj->parent = parent;
  return obj;
}

Object *alloc_int(Object *context, int value) {
  Object *root = context;
  while (root->parent) root = root->parent;
  Object *int_base = object_lookup(root, "int", NULL);
  IntObject *obj = alloc_object_internal(context, sizeof(IntObject));
  obj->base.parent = int_base;
  // why though?
  // obj->base.flags |= OBJ_IMMUTABLE | OBJ_CLOSED;
  obj->value = value;
  return (Object*) obj;
}

Object *alloc_bool(Object *context, int value) {
  Object *root = context;
  while (root->parent) root = root->parent;
  Object *bool_base = object_lookup(root, "bool", NULL);
  BoolObject *obj = alloc_object_internal(context, sizeof(BoolObject));
  obj->base.parent = bool_base;
  // obj->base.flags |= OBJ_IMMUTABLE | OBJ_CLOSED;
  obj->value = value;
  return (Object*) obj;
}

Object *alloc_float(Object *context, float value) {
  Object *root = context;
  while (root->parent) root = root->parent;
  Object *float_base = object_lookup(root, "float", NULL);
  FloatObject *obj = alloc_object_internal(context, sizeof(FloatObject));
  obj->base.parent = float_base;
  // obj->base.flags |= OBJ_IMMUTABLE | OBJ_CLOSED;
  obj->value = value;
  return (Object*) obj;
}

Object *alloc_string(Object *context, char *value) {
  Object *root = context;
  while (root->parent) root = root->parent;
  Object *string_base = object_lookup(root, "string", NULL);
  int len = strlen(value);
  // allocate the string as part of the object, so that it gets freed with the object
  StringObject *obj = alloc_object_internal(context, sizeof(StringObject) + len + 1);
  obj->base.parent = string_base;
  // obj->base.flags |= OBJ_IMMUTABLE | OBJ_CLOSED;
  obj->value = ((char*) obj) + sizeof(StringObject);
  strncpy(obj->value, value, len + 1);
  return (Object*) obj;
}

Object *alloc_fn(Object *context, VMFunctionPointer fn) {
  Object *root = context;
  while (root->parent) root = root->parent;
  Object *fn_base = object_lookup(root, "function", NULL);
  FunctionObject *obj = alloc_object_internal(context, sizeof(FunctionObject));
  obj->base.parent = fn_base;
  obj->fn_ptr = fn;
  return (Object*) obj;
}
