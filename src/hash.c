#include "hash.h"

#include <assert.h>
#include <string.h>

void *cache_alloc(int size);

void cache_free(int size, void *ptr);

// thanks http://stackoverflow.com/questions/7666509/hash-function-for-string
static unsigned long djb2(const char *str) {
  unsigned long hash = 5381;
  int c;
  while ((c = *str++) != 0) hash = ((hash << 5) + hash) + c; /* hash * 33 + c */
  return hash;
} 

void **table_lookup_ref(HashTable *tbl, const char *key) {
  assert(key != NULL);
  if (tbl->entries_stored == 0) return NULL;
  // printf(":: %s into %i having %i\n", key, tbl->entries_num, tbl->entries_stored);
  int entries_num = tbl->entries_num;
  if (entries_num <= 4) { // faster to do a direct scan
    for (int i = 0; i < entries_num; ++i) {
      TableEntry *entry = &tbl->entries_ptr[i];
      if (entry->name
        && key[0] == entry->name[0]
        && (!key[0] || key[1] == entry->name[1])
        && strcmp(key, entry->name) == 0
      ) return &entry->value;
    }
  } else {
    int entries_mask = entries_num - 1;
    int base = djb2(key);
    for (int i = 0; i < entries_num; ++i) {
      int k = (base + i) & entries_mask;
      TableEntry *entry = &tbl->entries_ptr[k];
      if (entry->name == NULL) return NULL;
      if (key[0] == entry->name[0]
        && (!key[0] || key[1] == entry->name[1])
        && strcmp(key, entry->name) == 0
      ) return &entry->value;
    }
  }
  // scanned the complete table; this really should not have happened
  return NULL;
}

// if the key was not found, return null but allocate a mapping in first_free_ptr
void **table_lookup_ref_alloc(HashTable *tbl, const char *key, void*** first_free_ptr) {
  assert(key != NULL);
  *first_free_ptr = NULL;
  // printf(":: %s into %i having %i\n", key, tbl->entries_num, tbl->entries_stored);
  int entries_num = tbl->entries_num;
  int entries_mask = entries_num - 1;
  int base = entries_num?djb2(key):0;
  TableEntry *free_ptr;
  for (int i = 0; i < entries_num; ++i) {
    int k = (base + i) & entries_mask;
    TableEntry *entry = &tbl->entries_ptr[k];
    if (entry->name
      && key[0] == entry->name[0]
      && (!key[0] || key[1] == entry->name[1])
      && strcmp(key, entry->name) == 0
    ) return &entry->value;
    if (entry->name == NULL) {
      free_ptr = entry;
      break;
    }
  }
  int newlen;
  if (entries_num == 0) {
    // definitely resize
    newlen = 4;
  } else {
    int fillrate = (tbl->entries_stored * 100) / entries_num;
    if (fillrate < 70) {
      // printf("--%p:   fillrate is okay with %i, set %li to %s\n", (void*) tbl, fillrate, free_ptr - tbl->entries_ptr, key);
      assert(free_ptr); // should have been found above
      free_ptr->name = key;
      tbl->entries_stored ++;
      *first_free_ptr = &free_ptr->value;
      return NULL;
    }
    newlen = entries_num * 2;
  }
  HashTable newtable;
  newtable.entries_ptr = cache_alloc(sizeof(TableEntry) * newlen);
  newtable.entries_num = newlen;
  newtable.entries_stored = 0;
  for (int i = 0; i < entries_num; ++i) {
    TableEntry *entry = &tbl->entries_ptr[i];
    if (entry->name) {
      void **freeptr;
      void **nope = table_lookup_ref_alloc(&newtable, entry->name, &freeptr);
      assert(nope == NULL); // double entry??
      *freeptr = entry->value;
    }
  }
  cache_free(sizeof(TableEntry) * tbl->entries_num, tbl->entries_ptr);
  *tbl = newtable;
  // and redo with new size!
  return table_lookup_ref_alloc(tbl, key, first_free_ptr);
}

void *table_lookup(HashTable *tbl, const char *key, bool *key_found_p) {
  void **ptr = table_lookup_ref(tbl, key);
  if (ptr == NULL) { if (key_found_p) *key_found_p = false; return NULL; }
  if (key_found_p) *key_found_p = true;
  return *ptr;
}
