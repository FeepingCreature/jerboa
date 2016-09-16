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
  // printf(":: %.*s\n", (int) key_len, key_ptr);
  int entries_num = tbl->entries_num;
  // printf("::%.*s in %i\n", key_len, key_ptr, entries_num);
  size_t entries_mask = entries_num - 1;
  int early_index = key->last_index & entries_mask;
  TableEntry *early_entry = &tbl->entries_ptr[early_index];
  if (LIKELY(early_entry->key.ptr == key->ptr)) {
    return early_entry;
  }
  
  size_t k = key->hash & entries_mask;
  while (true) {
    TableEntry *entry = &tbl->entries_ptr[k];
    if (entry->key.ptr == NULL) break;
    if (key->ptr == entry->key.ptr) {
      key->last_index = k;
      return entry;
    }
    k = (k + 1) & entries_mask;
    if (k == key->hash) break;
  }
  return NULL;
}

TableEntry *table_lookup_prepared(HashTable *tbl, FastKey *key) {
  return table_lookup_prepared_internal(tbl, key);
}

// if the key was not found, return null but allocate a mapping in first_free_ptr
TableEntry *table_lookup(HashTable *tbl, const char *key_ptr, size_t key_len) {
  FastKey key = prepare_key(key_ptr, key_len);
  return table_lookup_prepared_internal(tbl, &key);
}

void create_table_with_single_entry_prepared(HashTable *tbl, FastKey key, Value value) {
  assert(tbl->entries_num == 0);
  tbl->entries_ptr = cache_alloc_uninitialized(sizeof(TableEntry) * 1);
  tbl->entries_num = 1;
  tbl->entries_stored = 1;
  tbl->bloom = key.hash;
  tbl->entries_ptr[0] = (TableEntry) {
    .key = key,
    .value = value,
    .constraint = NULL
  };
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
    if (LIKELY(early_entry->key.ptr == key->ptr)) {
      return early_entry;
    }
    
    TableEntry *free_ptr = NULL;
    for (int i = 0; i < entries_num; ++i) {
      int k = (key->hash + i) & entries_mask;
      TableEntry *entry = &tbl->entries_ptr[k];
      if (entry->key.ptr == key->ptr) return entry;
      if (entry->key.ptr == NULL) {
        free_ptr = entry;
        break;
      }
    }
    int fillrate = (tbl->entries_stored * 100) / entries_num;
    if (fillrate < 70) {
      // printf("--%p:   fillrate is okay with %i, set %li to %s\n", (void*) tbl, fillrate, free_ptr - tbl->entries_ptr, key);
      assert(free_ptr); // should have been found above
      free_ptr->key = *key;
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
      if (entry->key.ptr) {
        TableEntry *freeptr;
        TableEntry *nope = table_lookup_alloc_prepared_internal(&newtable, &entry->key, &freeptr);
        if (UNLIKELY(nope)) {
          fprintf(stderr, "problem: %i '%.*s' %p already in table when reallocating\n", (int) entry->key.len, (int) entry->key.len, entry->key.ptr, entry->key.ptr);
        }
        (void) nope; assert(nope == NULL); // double entry??
        *freeptr = tbl->entries_ptr[i];
      }
    }
  }
  if (tbl->entries_ptr) cache_free(sizeof(TableEntry) * tbl->entries_num, tbl->entries_ptr);
  *tbl = newtable;
  // and redo with new size!
  return table_lookup_alloc_prepared(tbl, key, first_free_ptr);
}

TableEntry *table_lookup_alloc_prepared(HashTable *tbl, FastKey *key, TableEntry** first_free_ptr) {
  return table_lookup_alloc_prepared_internal(tbl, key, first_free_ptr);
}

// if the key was not found, return null but allocate a mapping in first_free_ptr
TableEntry *table_lookup_alloc(HashTable *tbl, const char *key_ptr, size_t key_len, TableEntry** first_free_ptr) {
  FastKey key = prepare_key(key_ptr, key_len);
  return table_lookup_alloc_prepared_internal(tbl, &key, first_free_ptr);
}
