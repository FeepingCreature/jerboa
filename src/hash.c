#include "hash.h"
#include "trie.h"

#include <assert.h>
#include <string.h>
#include <stddef.h>
#include <stdio.h>

void *cache_alloc_uninitialized(int size);

void cache_free(int size, void *ptr);

// look up a free key and return a pointer to it
// used for faster upsizing
static inline TableEntry *table_lookup_free_prepared_internal_plain(HashTable * __restrict__ tbl, size_t hash) {
  int entries_num = tbl->entries_num;
  size_t entries_mask = entries_num - 1;
  size_t k = hash & entries_mask;
  size_t initial_k = k;
  while (true) {
    TableEntry * __restrict__ entry = &tbl->entries_ptr[k];
    assert(entry->hash != hash);
    if (entry->hash == 0) return entry;
    k = (k + 1) & entries_mask;
    (void) initial_k; assert(k != initial_k);
  }
}

static inline TableEntry *table_lookup_prepared_internal(HashTable * __restrict__ tbl, FastKey * __restrict__ key) {
  // implied by the bloom test, since key->hash is never 0
  // if (tbl->entries_stored == 0) return NULL;
  if ((tbl->bloom & key->hash) != key->hash) return NULL;
  // printf(":: %.*s\n", (int) key->len, key->ptr);
  int entries_num = tbl->entries_num;
  // printf("::%.*s in %i\n", key_len, key_ptr, entries_num);
  size_t entries_mask = entries_num - 1;
  size_t early_index = key->last_index & entries_mask;
  TableEntry * __restrict__ early_entry = &tbl->entries_ptr[early_index];
  // approximately four times as likely, according to profiling
  if (LIKELY(early_entry->hash == key->hash)) {
    return early_entry;
  }
  
  size_t k = key->hash & entries_mask;
  size_t initial_k = k;
  // partial unroll up to n=4
  {
    TableEntry * __restrict__ entry = &tbl->entries_ptr[k];
    if (entry->hash == 0) return NULL; // can never happen naturally
    if (entry->hash == key->hash) {
      key->last_index = k;
      return entry;
    }
    if (entries_num == 1) return NULL;
    k = (k + 1) & entries_mask;
  }
  {
    TableEntry * __restrict__ entry = &tbl->entries_ptr[k];
    if (entry->hash == 0) return NULL;
    if (entry->hash == key->hash) {
      key->last_index = k;
      return entry;
    }
    if (entries_num == 2) return NULL;
    k = (k + 1) & entries_mask;
  }
  {
    TableEntry * __restrict__ entry = &tbl->entries_ptr[k];
    if (entry->hash == 0) return NULL;
    if (entry->hash == key->hash) {
      key->last_index = k;
      return entry;
    }
    k = (k + 1) & entries_mask;
  }
  {
    TableEntry * __restrict__ entry = &tbl->entries_ptr[k];
    if (entry->hash == 0) return NULL;
    if (entry->hash == key->hash) {
      key->last_index = k;
      return entry;
    }
    if (entries_num == 4) return NULL;
    k = (k + 1) & entries_mask;
  }
  while (true) {
    TableEntry * __restrict__ entry = &tbl->entries_ptr[k];
    // fprintf(stderr, "loop %li of %i: %p vs. %p\n", k, entries_num, entry->key_ptr, key->ptr);
    if (entry->hash == 0) break;
    if (entry->hash == key->hash) {
      key->last_index = k;
      return entry;
    }
    k = (k + 1) & entries_mask;
    if (k == initial_k) break;
  }
  return NULL;
}

TableEntry *table_lookup_prepared(HashTable *tbl, FastKey *key) {
  return table_lookup_prepared_internal(tbl, key);
}

void create_table_with_single_entry_prepared(HashTable *tbl, FastKey key, Value value) {
  assert(tbl->entries_num == 0);
  tbl->entries_ptr = cache_alloc_uninitialized(sizeof(TableEntry) * 1);
  tbl->entries_num = 1;
  tbl->entries_stored = 1;
  tbl->bloom = key.hash;
  tbl->entries_ptr[0] = (TableEntry) {
    .constraint = NULL,
    .hash = key.hash,
    .value = value
  };
}

void table_free(HashTable *tbl) {
  if (tbl->entries_ptr) {
    cache_free(sizeof(TableEntry) * tbl->entries_num, tbl->entries_ptr);
  }
}

static TableEntry *table_lookup_alloc_prepared_internal(HashTable *tbl, FastKey *key, TableEntry** first_free_ptr) {
  *first_free_ptr = NULL;
  // else is a realloc
  // if (key->key) printf(":: %s into %i having %i\n", key->key, tbl->entries_num, tbl->entries_stored);
  int entries_num = tbl->entries_num;
  int entries_mask = entries_num - 1;
  int newlen;
  if (entries_num == 0) {
    // definitely resize
    newlen = 1;
  } else {
    int early_index = key->last_index & entries_mask;
    TableEntry *early_entry = &tbl->entries_ptr[early_index];
    if (LIKELY(early_entry->hash == key->hash)) {
      return early_entry;
    }
    
    TableEntry *free_ptr = NULL;
    int k = key->hash & entries_mask, initial_k = k;
    while (true) {
      TableEntry *entry = &tbl->entries_ptr[k];
      if (entry->hash == key->hash) return entry;
      if (entry->hash == 0) {
        free_ptr = entry;
        break;
      }
      k = (k + 1) & entries_mask;
      if (k == initial_k) break;
    }
    int fillrate = (tbl->entries_stored * 100) / entries_num;
    if (fillrate < 70) {
      // printf("--%p:   fillrate is okay with %i, set %li to %s\n", (void*) tbl, fillrate, free_ptr - tbl->entries_ptr, key);
      assert(free_ptr); // should have been found above
      free_ptr->hash = key->hash;
      tbl->entries_stored ++;
      tbl->bloom |= key->hash;
      *first_free_ptr = free_ptr;
      return NULL;
    }
    newlen = entries_num * 2;
  }
  HashTable newtable;
  newtable.entries_ptr = cache_alloc_uninitialized(sizeof(TableEntry) * newlen);
  bzero(newtable.entries_ptr, sizeof(TableEntry) * newlen);
  newtable.entries_num = newlen;
  newtable.entries_stored = tbl->entries_stored;
  newtable.bloom = tbl->bloom;
  if (tbl->entries_stored) {
    for (int i = 0; i < entries_num; ++i) {
      TableEntry *entry = &tbl->entries_ptr[i];
      if (entry->hash) {
        TableEntry *freeptr = table_lookup_free_prepared_internal_plain(&newtable, entry->hash);
        *freeptr = tbl->entries_ptr[i];
      }
    }
  }
  table_free(tbl);
  *tbl = newtable;
  // and redo with new size!
  return table_lookup_alloc_prepared(tbl, key, first_free_ptr);
}

TableEntry *table_lookup_alloc_prepared(HashTable *tbl, FastKey *key, TableEntry** first_free_ptr) {
  return table_lookup_alloc_prepared_internal(tbl, key, first_free_ptr);
}
