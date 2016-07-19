#ifndef VM_CFG_H
#define VM_CFG_H

#include "vm/instr.h"

typedef struct {
  int pred_len; int *pred_ptr;
  int succ_len; int *succ_ptr;
} CFGNode;

typedef struct {
  int nodes_len;
  CFGNode *nodes_ptr;
  int *_succ_edgelist_ptr, *_pred_edgelist_ptr; // for freeing later
} CFG;

typedef struct {
  int len;
  int *ptr;
} Node2RPost;

typedef struct {
  int len;
  int *ptr;
} RPost2Node;

void cfg_build(CFG *cfg, UserFunction *uf);

void cfg_destroy(CFG *cfg);

void cfg_dump(char *file, CFG *cfg);

RPost2Node cfg_get_reverse_postorder(CFG *cfg);

Node2RPost cfg_invert_rpost(CFG *cfg, RPost2Node rpost2node);

void cfg_build_sfidom_list(CFG *cfg, RPost2Node rpost2node, Node2RPost node2rpost, int **sfidoms_ptr_p);

#endif
