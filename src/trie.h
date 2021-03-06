#ifndef JERBOA_TRIE_H
#define JERBOA_TRIE_H

#include <stdio.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

typedef enum {
  TRIE_NODE,
  TRIE_LEAF
} TrieNodeType;

typedef struct _TrieNode TrieNode;

typedef struct {
  size_t num_entries;
  TrieNode ***entries; // two-tier table, 4+4 bits of first character
} TrieNodeData;

typedef struct {
  uint32_t hash;
  const char *key;
} TrieLeafData;

struct _TrieNode {
  TrieNodeType type;
  const char *key_ptr;
  size_t key_len;
  union {
    TrieNodeData node;
    TrieLeafData leaf;
  };
};

void trie_dump_intern(FILE *file);

TrieNode *trie_insert(TrieNode *target, const char *key_ptr, size_t key_len, uint32_t hash, const char *key);

bool trie_lookup(TrieNode *node, const char *key_ptr, size_t key_len, uint32_t *hash_p, const char **key_p);

const char *trie_reverse_lookup(uint32_t hash);

#endif
