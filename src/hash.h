#ifndef HASH_H
#define HASH_H

#include <stddef.h>
#include <stdbool.h>

#define UNLIKELY(X) __builtin_expect(X, 0)

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
  size_t bloom;
} HashTable;

void **table_lookup_ref(HashTable *tbl, const char *key_ptr, int key_len) __attribute__ ((pure));

void **table_lookup_ref_with_hash(HashTable *tbl, const char *key_ptr, int key_len, size_t key_hash) __attribute__ ((pure));

// if the key was not found, return null but allocate a mapping in first_free_ptr
void **table_lookup_ref_alloc(HashTable *tbl, const char *key_ptr, int key_len, void*** first_free_ptr);

// added in case you've already precomputed the hash for other reasons, and wanna avoid double computing it
void **table_lookup_ref_alloc_with_hash(HashTable *tbl, const char *key_ptr, int key_len, size_t key_hash, void*** first_free_ptr);

void *table_lookup(HashTable *tbl, const char *key_ptr, int key_len, bool *key_found_p);

void *table_lookup_with_hash(HashTable *tbl, const char *key_ptr, int key_len, size_t hashv, bool *key_found_p);

// thanks http://stackoverflow.com/questions/7666509/hash-function-for-string
static inline size_t hash(const char *ptr, int len) {
  size_t hash = 5381;
  for (int i = 0; i < len; ++i) {
    hash = hash * 33 + ptr[i];
  }
  return hash;
} 

#endif
