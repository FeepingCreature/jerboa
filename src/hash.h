#ifndef HASH_H
#define HASH_H

#include <stddef.h>
#include <stdbool.h>

struct _TableEntry;
typedef struct _TableEntry TableEntry;

struct _TableEntry {
  const char *name_ptr;
  size_t name_len;
  void *value;
};

// TODO actually use
#define TBL_GRAVESTONE = ((const char*) -1);

typedef struct {
  TableEntry *entries_ptr;
  int entries_num;
  int entries_stored;
} HashTable;

void **table_lookup_ref(HashTable *tbl, const char *key_ptr, int key_len);

// if the key was not found, return null but allocate a mapping in first_free_ptr
void **table_lookup_ref_alloc(HashTable *tbl, const char *key_ptr, int key_len, void*** first_free_ptr);

void *table_lookup(HashTable *tbl, const char *key_ptr, int key_len, bool *key_found_p);

#endif
