#include "hash.h"
#include "trie.h"

#include <assert.h>
#include <string.h>
#include <stddef.h>

void *cache_alloc_uninitialized(int size);

void cache_free(int size, void *ptr);

#include <stdio.h>
static inline TableEntry *table_lookup_prepared_internal(HashTable *tbl, FastKey *key) {
  assert(key->ptr != NULL);
  // implied by the bloom test, since key->hash is never 0
  // if (tbl->entries_stored == 0) return NULL;
  if (LIKELY((tbl->bloom & key->hash) != key->hash)) return NULL;
  // printf(":: %.*s\n", (int) key->len, key->ptr);
  int entries_num = tbl->entries_num;
  // printf("::%.*s in %i\n", key_len, key_ptr, entries_num);
  size_t entries_mask = entries_num - 1;
  size_t early_index = key->last_index & entries_mask;
  TableEntry *early_entry = &tbl->entries_ptr[early_index];
  if (LIKELY(early_entry->key_ptr == key->ptr)) {
    return early_entry;
  }
  
  size_t k = key->hash & entries_mask;
  while (true) {
    TableEntry *entry = &tbl->entries_ptr[k];
    if (entry->key_ptr == NULL) break;
    if (key->ptr == entry->key_ptr) {
      key->last_index = k;
      return entry;
    }
    k = (k + 1) & entries_mask;
    if (k == (key->hash & entries_mask)) break;
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
    .key_ptr = key.ptr,
    .hash = key.hash,
    .value = value,
    .constraint = NULL
  };
}

void table_free(HashTable *tbl) {
  if (tbl->entries_ptr) {
    cache_free(sizeof(TableEntry) * tbl->entries_num, tbl->entries_ptr);
  }
}

static TableEntry *table_lookup_alloc_prepared_internal(HashTable *tbl, FastKey *key, TableEntry** first_free_ptr) {
  *first_free_ptr = NULL;
  // printf(":: %.*s into %i having %i\n", key_len, key_ptr, tbl->entries_num, tbl->entries_stored);
  int entries_num = tbl->entries_num;
  int entries_mask = entries_num - 1;
  int newlen;
  if (entries_num == 0) {
    // definitely resize
    newlen = 4;
  } else {
    int early_index = key->last_index & entries_mask;
    TableEntry *early_entry = &tbl->entries_ptr[early_index];
    if (LIKELY(early_entry->key_ptr == key->ptr)) {
      return early_entry;
    }
    
    TableEntry *free_ptr = NULL;
    for (int i = 0; i < entries_num; ++i) {
      int k = (key->hash + i) & entries_mask;
      TableEntry *entry = &tbl->entries_ptr[k];
      if (entry->key_ptr == key->ptr) return entry;
      if (entry->key_ptr == NULL) {
        free_ptr = entry;
        break;
      }
    }
    int fillrate = (tbl->entries_stored * 100) / entries_num;
    if (fillrate < 70) {
      // printf("--%p:   fillrate is okay with %i, set %li to %s\n", (void*) tbl, fillrate, free_ptr - tbl->entries_ptr, key);
      assert(free_ptr); // should have been found above
      free_ptr->key_ptr = key->ptr;
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
  newtable.entries_stored = 0;
  newtable.bloom = 0;
  if (tbl->entries_stored) {
    for (int i = 0; i < entries_num; ++i) {
      TableEntry *entry = &tbl->entries_ptr[i];
      if (entry->key_ptr) {
        TableEntry *freeptr;
        FastKey cur_key = (FastKey) { .ptr = entry->key_ptr, .hash = entry->hash };
        TableEntry *nope = table_lookup_alloc_prepared_internal(&newtable, &cur_key, &freeptr);
        if (UNLIKELY(nope)) {
          fprintf(stderr, "problem: '%s' %p already in table when reallocating\n", entry->key_ptr, (void*) entry->key_ptr);
        }
        (void) nope; assert(nope == NULL); // double entry??
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
