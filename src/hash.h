#ifndef JERBOA_HASH_H
#define JERBOA_HASH_H

#include <stddef.h>
#include <stdbool.h>
#include "core.h"

FastKey prepare_key(const char *key_ptr, size_t key_len);

// pretend "ptr" is an already-interned char pointer
// used for the profiler hack
FastKey fixed_pointer_key(void *ptr);

TableEntry *table_lookup_prepared(HashTable *tbl, FastKey *key) __attribute__ ((pure));

// added in case you've already precomputed the hash for other reasons, and wanna avoid double computing it
TableEntry *table_lookup_alloc_prepared(HashTable *tbl, FastKey *key, TableEntry** first_free_ptr);

// fastpath: for creation of {"this"} object
void create_table_with_single_entry_prepared(HashTable *tbl, FastKey key, Value value);

// thanks http://stackoverflow.com/questions/7666509/hash-function-for-string
// note: NEVER returns 0!
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
  if (hash == 0) hash = 1; // this makes some other code safer
  return hash;
} 

#endif
