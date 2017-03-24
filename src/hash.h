#ifndef JERBOA_HASH_H
#define JERBOA_HASH_H

#include <stddef.h>
#include <stdbool.h>
#include "core.h"

// NOTE: if you ever add code to remove keys from a table, it is VITAL that the bloom filter be reset to zero at the last key!
// otherwise, lookups will break badly!

FastKey prepare_key(const char *key_ptr, size_t key_len);

// pretend "ptr" is an already-interned char pointer
// used for the profiler hack
FastKey fixed_pointer_key(void *ptr);

TableEntry *table_lookup_prepared(HashTable *tbl, FastKey *key) __attribute__ ((pure));

// added in case you've already precomputed the hash for other reasons, and wanna avoid double computing it
TableEntry *table_lookup_alloc_prepared(HashTable *tbl, FastKey *key, TableEntry** first_free_ptr);

// fastpath: for creation of {"this"} object
void create_table_with_single_entry_prepared(HashTable *tbl, FastKey key, Value value);

void table_free(HashTable *tbl);

#endif
