#include "trie.h"

#include "hash.h"
#include "core.h"
#include "util.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

// TODO sync
static void *freeslab = NULL; // should be 1-2MB, so allocate in MB-size steps
static int freeslab_size_left = 0;
static void *trie_alloc_uninitialized(int size) {
  assert(size <= 1024*1024);
  if (UNLIKELY(freeslab_size_left < size)) {
    freeslab = malloc(1024*1024);
    freeslab_size_left = 1024*1024;
  }
  void *res = freeslab;
  freeslab = (void*) ((char*) freeslab + size);
  freeslab_size_left -= size;
  return res;
}

static void *trie_alloc(int size) {
  void *res = trie_alloc_uninitialized(size);
  bzero(res, size);
  return res;
}

static size_t common_len(const char *a_ptr, size_t a_len, const char *b_ptr, size_t b_len) {
  size_t i = 0;
  while (i < a_len && i < b_len && a_ptr[i] == b_ptr[i]) i++;
  return i;
}

static void trie_dump_internal(FILE *file, TrieNode *node, int spacing) {
  if (node == NULL) fprintf(file, "<null>");
  else if (node->type == TRIE_LEAF) {
    for (int i = 0; i < spacing; i++) fprintf(file, "| ");
    fprintf(file, "-<%.*s: %i>\n", (int) node->key_len, node->key_ptr, node->leaf.hash);
  } else {
    assert(node->type == TRIE_NODE);
    for (int i = 0; i < spacing; i++) fprintf(file, "| ");
    fprintf(file, "+%.*s|\n", (int) node->key_len, node->key_ptr);
    if (node->node.entries) {
      for (int i = 0; i < 16; i++) {
        TrieNode **sublist = node->node.entries[i];
        if (sublist) {
          for (int k = 0; k < 16; ++k) {
            TrieNode *child = sublist[k];
            if (child) {
              trie_dump_internal(file, child, spacing + 1);
            }
          }
        }
      }
    }
  }
}

static TrieNode *trie_insert_node(TrieNode *target, TrieNode *node);

static TrieNode *trie_insert_into_node(TrieNode *target, TrieNode *node) {
  assert(target->type == TRIE_NODE);
  
  size_t old_key_size = target->key_len;
  size_t new_key_size;
  if (target->key_ptr) {
    new_key_size = common_len(target->key_ptr, target->key_len, node->key_ptr, node->key_len);
  } else {
    target->key_ptr = node->key_ptr;
    new_key_size = node->key_len;
  }
  ptrdiff_t key_delta = new_key_size - old_key_size;
  
  // inserting would shorten our shared key, so split this node off into a separate subnode
  if (key_delta < 0 && target->node.num_entries > 1) {
    TrieNode *new_node = trie_alloc_uninitialized(sizeof(TrieNode));
    *new_node = (TrieNode) {TRIE_NODE, NULL, 0, .node = {0}};
    new_node = trie_insert_into_node(new_node, target);
    new_node = trie_insert_into_node(new_node, node);
    return new_node;
  }
  
  // adjust node's key
  // fprintf(stderr, "shorten %.*s by %i: '%.*s'\n",
  //         (int) node->key_len, node->key_ptr, (int) new_key_size, (int) (node->key_len - new_key_size), node->key_ptr + new_key_size);
  node->key_ptr += new_key_size;
  node->key_len -= new_key_size;
  // adjust target's key
  target->key_len = new_key_size;
  
  unsigned char index;
  if (node->key_len > 0) {
    index = node->key_ptr[0];
    assert(index > 0);
  } else {
    index = 0;
  }
  unsigned char index0 = index >> 4, index1 = index & 0xf;
  // fprintf(stderr, "%.*s: index is %i: %i, %i delta %i\n", (int) node->key_len, node->key_ptr, index, index0, index1, (int) key_delta);
  
  if (key_delta > 0) {
    // first insertion
    assert(target->node.num_entries == 0);
  }
  if (key_delta < 0) {
    assert(target->node.num_entries == 1);
    assert(target->node.entries[0][0]); // one entry: [foo| <''>]
    TrieNode *old_entry = target->node.entries[0][0];
    old_entry->key_ptr += key_delta;
    old_entry->key_len -= key_delta;
    
    unsigned char new_index;
    if (old_entry->key_len > 0) {
      new_index = old_entry->key_ptr[0];
      assert(new_index > 0);
    } else {
      new_index = 0;
    }
    unsigned char new_index0 = new_index >> 4, new_index1 = new_index & 0xf;
    target->node.entries[0][0] = 0;
    if (!target->node.entries[new_index0]) target->node.entries[new_index0] = trie_alloc(sizeof(TrieNode*) * 16);
    assert(target->node.entries[new_index0][new_index1] == NULL);
    target->node.entries[new_index0][new_index1] = old_entry;
  }
  
  if (!target->node.entries) target->node.entries = trie_alloc(sizeof(TrieNode**) * 16);
  if (!target->node.entries[index0]) target->node.entries[index0] = trie_alloc(sizeof(TrieNode*) * 16);
  TrieNode *child = target->node.entries[index0][index1];
  if (child) {
    size_t common = common_len(child->key_ptr, child->key_len, node->key_ptr, node->key_len);
    if (common > 0) {
      target->node.entries[index0][index1] = trie_insert_node(child, node);
      return target;
    } else {
      fprintf(stderr, "slot is taken, but no characters in common??\n");
      abort();
    }
  }
  assert(target->node.entries[index0][index1] == NULL);
  target->node.entries[index0][index1] = node;
  target->node.num_entries ++;
  return target;
}

static TrieNode *trie_insert_node(TrieNode *target, TrieNode *node) {
  TrieNode *new_node;
  if (target == NULL) {
    new_node = node;
  } else if (target->type == TRIE_LEAF) {
    new_node = trie_alloc_uninitialized(sizeof(TrieNode));
    *new_node = (TrieNode) {TRIE_NODE, NULL, 0, .node = {0}};
    new_node = trie_insert_into_node(new_node, target);
    new_node = trie_insert_into_node(new_node, node);
  } else {
    assert(target->type == TRIE_NODE);
    new_node = trie_insert_into_node(target, node);
  }
  return new_node;
}

TrieNode *trie_insert(TrieNode *target, const char *key_ptr, size_t key_len, const uint32_t hash, const char *key) {
  TrieNode *new_leaf = trie_alloc_uninitialized(sizeof(TrieNode));
  *new_leaf = (TrieNode) {TRIE_LEAF, key_ptr, key_len, .leaf = {hash, key}};
  return trie_insert_node(target, new_leaf);
}

bool trie_lookup(TrieNode * __restrict__ node, const char *key_ptr, size_t key_len, uint32_t *hash_p, const char **key_p) {
  if (!node) return false;
  // printf(": '%.*s' against '%.*s'\n", (int) key_len, key_ptr, (int) node->key_len, node->key_ptr);
  if (node->key_len > key_len) return false;
  if (strncmp(node->key_ptr, key_ptr, node->key_len) != 0) return false;
  // eat the matched prefix
  key_ptr += node->key_len;
  key_len -= node->key_len;
  
  if (node->type == TRIE_LEAF) {
    if (key_len > 0) return false;
    *hash_p = node->leaf.hash;
    *key_p = node->leaf.key;
    return true;
  } else {
    assert(node->type == TRIE_NODE);
    
    unsigned char index;
    if (key_len > 0) {
      index = key_ptr[0];
      assert(index > 0);
    } else {
      index = 0;
    }
    unsigned char index0 = index >> 4, index1 = index & 0xf;
    if (!node->node.entries) return false;
    if (!node->node.entries[index0]) return false;
    if (!node->node.entries[index0][index1]) return false;
    return trie_lookup(node->node.entries[index0][index1], key_ptr, key_len, hash_p, key_p);
  }
}

// TODO mutex or make lockless (read/write lock? writes should be rare)
static TrieNode *intern_string_trie = NULL;
static uint32_t lcg_state = 1;
static HashTable intern_reverse = {0}; // unique hash value to string

void trie_dump_intern(FILE *file) {
  trie_dump_internal(file, intern_string_trie, 0);
}

FastKey prepare_key(const char *key_ptr, size_t key_len) {
  // printf(":: %.*s\n", (int) key_len, key_ptr);
  assert(key_ptr != NULL);
  // const char *old_ptr = key_ptr;
  // hash will be identical
  uint32_t hash; const char *key;
  if (!trie_lookup(intern_string_trie, key_ptr, key_len, &hash, &key)) {
    char *copy = trie_alloc_uninitialized(key_len + 1);
    memcpy(copy, key_ptr, key_len);
    copy[key_len] = 0;
    // fprintf(stderr, "+ %.*s\n", (int) key_len, key_ptr);
    lcg_state = lcg_parkmiller(lcg_state);
    if (UNLIKELY(lcg_state == 1)) {
      fprintf(stderr, "ran out of keys! how did you manage that, you had 2^31 to play with\n");
      abort();
    }
    hash = lcg_state;
    key = copy;
    intern_string_trie = trie_insert(intern_string_trie, copy, key_len, hash, key);
    
    FastKey fkey = { .hash = hash, .key = key };
    TableEntry *empty_entry = NULL;
    TableEntry *rev_entry = table_lookup_alloc_prepared(&intern_reverse, &fkey, &empty_entry);
    (void) rev_entry; assert(rev_entry == NULL); // existing entry for this hash? what
    empty_entry->value.obj = (Object*) key;
    // trie_dump(stderr, intern_string_trie);
    // fprintf(stderr, "------\n");
  }
  return (FastKey) {
    .hash = hash,
    .key = key
  };
}

const char *trie_reverse_lookup(uint32_t hash) {
  FastKey fkey = { .hash = hash };
  TableEntry *rev_entry = table_lookup_prepared(&intern_reverse, &fkey);
  assert(rev_entry != NULL);
  return (const char*) rev_entry->value.obj;
}
