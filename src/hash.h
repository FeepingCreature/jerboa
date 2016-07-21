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
  void *value_aux;
};

// TODO actually use
#define TBL_GRAVESTONE = ((const char*) -1);

typedef struct {
  TableEntry *entries_ptr;
  int entries_num;
  int entries_stored;
  size_t bloom;
} HashTable;

TableEntry *table_lookup(HashTable *tbl, const char *key_ptr, size_t key_len) __attribute__ ((pure));

TableEntry *table_lookup_with_hash(HashTable *tbl, const char *key_ptr, size_t key_len, size_t key_hash) __attribute__ ((pure));

// if the key was not found, return null but allocate a mapping in first_free_ptr
TableEntry *table_lookup_alloc(HashTable *tbl, const char *key_ptr, size_t key_len, TableEntry **first_free_ptr);

// added in case you've already precomputed the hash for other reasons, and wanna avoid double computing it
TableEntry *table_lookup_alloc_with_hash(HashTable *tbl, const char *key_ptr, size_t key_len, size_t key_hash, TableEntry** first_free_ptr);

// thanks http://stackoverflow.com/questions/7666509/hash-function-for-string
static inline size_t hash(const char *ptr, int len) {
  size_t hash = 5381;
  int i = 0;
  for (; i < (len &~7); i+=8) {
    hash = hash * 33 + ptr[i+0];
    hash = hash * 33 + ptr[i+1];
    hash = hash * 33 + ptr[i+2];
    hash = hash * 33 + ptr[i+3];
    hash = hash * 33 + ptr[i+4];
    hash = hash * 33 + ptr[i+5];
    hash = hash * 33 + ptr[i+6];
    hash = hash * 33 + ptr[i+7];
  }
  for (; i < len; i++) {
    hash = hash * 33 + ptr[i];
  }
  return hash;
} 

#endif
