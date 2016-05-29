#include "object.h"

#define DEBUG_MEM 0

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

#include <stdio.h>
void obj_claim(Object *obj) {
  // if (obj) fprintf(stderr, "[+] %i to %i\n", obj->id, obj->refs + 1);
  if (obj) { obj->refs ++; }
}

Object *obj_claimed(Object *obj) {
  obj_claim(obj);
  return obj;
}

void obj_free(Object *obj) {
  // if (obj) fprintf(stderr, "[-] %i to %i\n", obj->id, obj->refs - 1);
  if (obj && obj->refs == 0) abort();
  if (obj && --obj->refs == 0) {
#if DEBUG_MEM
    fprintf(stderr, "free object %i\n", obj->id);
#endif
    obj_free(obj->parent);
    TableEntry *entry = &obj->tbl.entry;
    bool start = true;
    while (entry) {
      obj_free(entry->value);
      
      TableEntry *next_entry = entry->next;
      if (start) start = false;
      else free(entry);
      entry = next_entry;
    }
    free(obj);
  }
}

// missing object/missing key == null
Object *object_lookup(Object *obj, char *key) {
  while (obj) {
    Object *value = table_lookup(&obj->tbl, key);
    if (value) return value;
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
      obj_free(*ptr);
      *ptr = obj_claimed(value);
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
    obj_free(*ptr);
  } else {
    assert(!(obj->flags & OBJ_CLOSED));
    freeptr->name = key;
    ptr = &freeptr->value;
  }
  *ptr = obj_claimed(value);
}

#if OBJ_KEEP_IDS
int idcounter = 0;
#endif

Object *alloc_object(Object *parent) {
  Object *obj = calloc(sizeof(Object), 1);
  obj->parent = obj_claimed(parent);
#if OBJ_KEEP_IDS
  obj->id = idcounter++;
#if DEBUG_MEM
  fprintf(stderr, "alloc object %i\n", obj->id);
#endif
#endif
  return obj;
}

Object *alloc_int(Object *context, int value) {
  Object *root = context;
  while (root->parent) root = root->parent;
  Object *int_base = object_lookup(root, "int");
  IntObject *obj = calloc(sizeof(IntObject), 1);
  obj->base.parent = obj_claimed(int_base);
#if OBJ_KEEP_IDS
  obj->base.id = idcounter++;
#if DEBUG_MEM
  fprintf(stderr, "alloc object %i\n", obj->base.id);
#endif
#endif
  obj->value = value;
  return (Object*) obj;
}

Object *alloc_bool(Object *context, int value) {
  Object *root = context;
  while (root->parent) root = root->parent;
  Object *bool_base = object_lookup(root, "bool");
  BoolObject *obj = calloc(sizeof(BoolObject), 1);
  obj->base.parent = obj_claimed(bool_base);
#if OBJ_KEEP_IDS
  obj->base.id = idcounter++;
#if DEBUG_MEM
  fprintf(stderr, "alloc object %i\n", obj->base.id);
#endif
#endif
  obj->value = value;
  return (Object*) obj;
}

Object *alloc_fn(Object *context, VMFunctionPointer fn) {
  Object *root = context;
  while (root->parent) root = root->parent;
  Object *fn_base = object_lookup(root, "function");
  FunctionObject *obj = calloc(sizeof(FunctionObject), 1);
  obj->base.parent = obj_claimed(fn_base);
#if OBJ_KEEP_IDS
  obj->base.id = idcounter++;
#if DEBUG_MEM
  fprintf(stderr, "alloc object %i\n", obj->base.id);
#endif
#endif
  obj->fn_ptr = fn;
  return (Object*) obj;
}
