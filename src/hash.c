#include "hash.h"

#include <assert.h>
#include <string.h>
#include <stddef.h>

void *cache_alloc(int size);

void cache_free(int size, void *ptr);

#include <stdio.h>
static inline TableEntry *table_lookup_with_hash_internal2(HashTable *tbl, const char *key_ptr, int key_len, size_t hashv) {
  assert(key_ptr != NULL);
  if (tbl->entries_stored == 0) return NULL;
  if ((tbl->bloom & hashv) != hashv) return NULL;
  // printf(":: %.*s into %i having %i\n", key_len, key_ptr, tbl->entries_num, tbl->entries_stored);
  // printf(":: %.*s\n", key_len, key_ptr);
  int entries_num = tbl->entries_num;
  // printf("::%.*s in %i\n", key_len, key_ptr, entries_num);
  if (entries_num <= 8) { // faster to do a direct scan
    for (int i = 0; i < entries_num; ++i) {
      TableEntry *entry = &tbl->entries_ptr[i];
      if (entry->name_len == key_len && (
          key_ptr == entry->name_ptr
          ||
          ((key_len == 0 || key_ptr[0] == entry->name_ptr[0])
            && (key_len <= 1 || key_ptr[1] == entry->name_ptr[1])
            && strncmp(key_ptr, entry->name_ptr, key_len) == 0
          )
      )) return entry;
    }
  } else {
    int entries_mask = entries_num - 1;
    for (int i = 0; i < entries_num; ++i) {
      int k = (hashv + i) & entries_mask;
      TableEntry *entry = &tbl->entries_ptr[k];
      if (entry->name_ptr == NULL) return NULL;
      if (entry->name_len == key_len && (
        key_ptr == entry->name_ptr
        ||
        ((key_len == 0 || key_ptr[0] == entry->name_ptr[0])
          && (key_len <= 1 || key_ptr[1] == entry->name_ptr[1])
          && strncmp(key_ptr, entry->name_ptr, key_len) == 0
        )
      )) return entry;
    }
  }
  // scanned the complete table; this really should not have happened
  return NULL;
}

static inline TableEntry *table_lookup_with_hash_internal(HashTable *tbl, const char *key_ptr, int key_len, size_t hashv) {
  if (UNLIKELY(key_len == 0)) return table_lookup_with_hash_internal2(tbl, key_ptr, 0, hashv);
  if (UNLIKELY(key_len == 1)) return table_lookup_with_hash_internal2(tbl, key_ptr, 1, hashv);
  if (UNLIKELY(key_len == 2)) return table_lookup_with_hash_internal2(tbl, key_ptr, 2, hashv);
  return table_lookup_with_hash_internal2(tbl, key_ptr, key_len, hashv);
}

TableEntry *table_lookup_with_hash(HashTable *tbl, const char *key_ptr, int key_len, size_t hashv) {
  return table_lookup_with_hash_internal(tbl, key_ptr, key_len, hashv);
}

// if the key was not found, return null but allocate a mapping in first_free_ptr
TableEntry *table_lookup(HashTable *tbl, const char *key_ptr, int key_len) {
  size_t hashv = (tbl->entries_num>8)?hash(key_ptr, key_len):0;
  return table_lookup_with_hash_internal(tbl, key_ptr, key_len, hashv);
}

static TableEntry *table_lookup_alloc_with_hash_internal(HashTable *tbl, const char *key_ptr, int key_len, size_t key_hash, TableEntry** first_free_ptr) {
  assert(key_ptr != NULL);
  *first_free_ptr = NULL;
  // printf(":: %.*s into %i having %i\n", key_len, key_ptr, tbl->entries_num, tbl->entries_stored);
  int entries_num = tbl->entries_num;
  int entries_mask = entries_num - 1;
  int newlen;
  if (entries_num == 0) {
    // definitely resize
    newlen = 4;
  } else {
    TableEntry *free_ptr = NULL;
    for (int i = 0; i < entries_num; ++i) {
      int k = (key_hash + i) & entries_mask;
      TableEntry *entry = &tbl->entries_ptr[k];
      if (entry->name_len == key_len
        && (entry->name_ptr == key_ptr
          ||
          ((key_len == 0 || key_ptr[0] == entry->name_ptr[0])
            && (key_len <= 1 || key_ptr[1] == entry->name_ptr[1])
            && strncmp(key_ptr, entry->name_ptr, key_len) == 0)
      )) return entry;
      if (entry->name_ptr == NULL) {
        free_ptr = entry;
        break;
      }
    }
    int fillrate = (tbl->entries_stored * 100) / entries_num;
    if (fillrate < 70) {
      // printf("--%p:   fillrate is okay with %i, set %li to %s\n", (void*) tbl, fillrate, free_ptr - tbl->entries_ptr, key);
      assert(free_ptr); // should have been found above
      free_ptr->name_ptr = key_ptr;
      free_ptr->name_len = key_len;
      tbl->entries_stored ++;
      tbl->bloom |= key_hash;
      *first_free_ptr = free_ptr;
      return NULL;
    }
    newlen = entries_num * 2;
  }
  HashTable newtable;
  newtable.entries_ptr = cache_alloc(sizeof(TableEntry) * newlen);
  newtable.entries_num = newlen;
  newtable.entries_stored = 0;
  newtable.bloom = 0;
  if (tbl->entries_stored) {
    for (int i = 0; i < entries_num; ++i) {
      TableEntry *entry = &tbl->entries_ptr[i];
      if (entry->name_ptr) {
        TableEntry *freeptr;
        TableEntry *nope = table_lookup_alloc(&newtable, entry->name_ptr, entry->name_len, &freeptr);
        (void) nope; assert(nope == NULL); // double entry??
        freeptr->value = entry->value;
      }
    }
  }
  if (tbl->entries_ptr) cache_free(sizeof(TableEntry) * tbl->entries_num, tbl->entries_ptr);
  *tbl = newtable;
  // and redo with new size!
  return table_lookup_alloc(tbl, key_ptr, key_len, first_free_ptr);
}

TableEntry *table_lookup_alloc_with_hash(HashTable *tbl, const char *key_ptr, int key_len, size_t key_hash, TableEntry** first_free_ptr) {
  return table_lookup_alloc_with_hash_internal(tbl, key_ptr, key_len, key_hash, first_free_ptr);
}

// if the key was not found, return null but allocate a mapping in first_free_ptr
TableEntry *table_lookup_alloc(HashTable *tbl, const char *key_ptr, int key_len, TableEntry** first_free_ptr) {
  size_t key_hash = tbl->entries_num?hash(key_ptr, key_len):0;
  return table_lookup_alloc_with_hash_internal(tbl, key_ptr, key_len, key_hash, first_free_ptr);
}
