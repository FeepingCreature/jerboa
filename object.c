#include "object.h"

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

Object *table_lookup(Table *tbl, char *key) {
  Object **ptr = table_lookup_ref_alloc(tbl, key, NULL);
  if (ptr == NULL) return NULL;
  return *ptr;
}

void table_set(Table *tbl, char *key, Object *value) {
  TableEntry *freeptr;
  Object **ptr = table_lookup_ref_alloc(tbl, key, &freeptr);
  if (ptr == NULL) {
    freeptr->name = key;
    ptr = &freeptr->value;
  }
  *ptr = value;
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

Object *alloc_object(Object *parent) {
  Object *obj = calloc(sizeof(Object), 1);
  obj->parent = parent;
  return obj;
}

Object *alloc_int(Object *context, int value) {
  Object *root = context;
  while (root->parent) root = root->parent;
  Object *int_base = table_lookup(&root->tbl, "int");
  IntObject *obj = calloc(sizeof(IntObject), 1);
  obj->base.parent = int_base;
  obj->value = value;
  return (Object*) obj;
}

Object *alloc_bool(Object *context, int value) {
  Object *root = context;
  while (root->parent) root = root->parent;
  Object *bool_base = table_lookup(&root->tbl, "bool");
  BoolObject *obj = calloc(sizeof(BoolObject), 1);
  obj->base.parent = bool_base;
  obj->value = value;
  return (Object*) obj;
}

Object *alloc_fn(Object *context, VMFunctionPointer fn) {
  Object *root = context;
  while (root->parent) root = root->parent;
  Object *fn_base = table_lookup(&root->tbl, "function");
  FunctionObject *obj = calloc(sizeof(FunctionObject), 1);
  obj->base.parent = fn_base;
  obj->fn_ptr = fn;
  return (Object*) obj;
}
