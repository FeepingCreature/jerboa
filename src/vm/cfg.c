#include "vm/cfg.h"

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <strings.h>

static int cfg_count_nodes_reachable(CFG *cfg, int index, bool *node_visited) {
  if (node_visited[index]) return 0;
  int res = 1;
  node_visited[index] = true;
  for (int i = 0; i < cfg->nodes_ptr[index].succ_len; ++i) {
    res += cfg_count_nodes_reachable(cfg, cfg->nodes_ptr[index].succ_ptr[i], node_visited);
  }
  return res;
}

static void cfg_walk_reverse_postorder(CFG *cfg, int nodeidx, int *res_ptr, int *residx, bool *node_visited) {
  if (node_visited[nodeidx]) return;
  node_visited[nodeidx] = true;
  // walk in reverse order, write in reverse order - cancels out
  for (int i = cfg->nodes_ptr[nodeidx].succ_len - 1; i >= 0; --i) {
    cfg_walk_reverse_postorder(cfg, cfg->nodes_ptr[nodeidx].succ_ptr[i], res_ptr, residx, node_visited);
  }
  res_ptr[--*residx] = nodeidx;
}

RPost2Node cfg_get_reverse_postorder(CFG *cfg) {
  // some nodes may be isolated from the graph for whatever reason
  // so do a preliminary count first
  RPost2Node res;
  
  bool *node_visited = calloc(sizeof(bool), cfg->nodes_len);
  
  int num_cfg_nodes = cfg_count_nodes_reachable(cfg, 0, node_visited);
  bzero(node_visited, sizeof(bool) * cfg->nodes_len);
  
  res.len = num_cfg_nodes;
  res.ptr = malloc(sizeof(int) * num_cfg_nodes);
  int i = num_cfg_nodes;
  cfg_walk_reverse_postorder(cfg, 0, res.ptr, &i, node_visited);
  assert(i == 0);
  free(node_visited);
  
  return res;
}

#define NodeId int
#define RPostId int

static int cfg_sfidom_intersect(int *sfidoms_ptr, RPostId a, RPostId b) {
  while (a != b) {
    while (a > b) a = sfidoms_ptr[a];
    while (b > a) b = sfidoms_ptr[b];
  }
  return a;
}

Node2RPost cfg_invert_rpost(CFG *cfg, RPost2Node rpost2node) {
  Node2RPost res;
  res.len = cfg->nodes_len;
  res.ptr = malloc(sizeof(int) * cfg->nodes_len);
  
  for (int i = 0; i < cfg->nodes_len; ++i) {
    res.ptr[i] = -1;
  }
  for (int i = 0; i < rpost2node.len; ++i) {
    res.ptr[rpost2node.ptr[i]] = i;
  }
  
  return res;
}

// Thank you https://www.cs.rice.edu/~keith/EMBED/dom.pdf !
int *cfg_build_sfidom_list(CFG *cfg, RPost2Node rpost2node, Node2RPost node2rpost) {
  // indexed in block order, not reverse post order!
  RPostId *sfidoms = malloc(sizeof(int) * rpost2node.len);
  for (int i = 0; i < rpost2node.len; ++i) {
    sfidoms[i] = -1; // undefined
  }
  sfidoms[node2rpost.ptr[0]] = node2rpost.ptr[0]; // start node
  
  while (true) {
    bool changed = false;
    // for all nodes in reverse post order...
    for (int i = 0; i < rpost2node.len; ++i) {
      NodeId block = rpost2node.ptr[i];
      CFGNode *cfgblk = &cfg->nodes_ptr[block];
      // except start node
      if (block == 0) continue;
      NodeId new_idom = -1;
      // for all predecessors of block
      for (int k = 0; k < cfgblk->pred_len; ++k) {
        NodeId pred_blk = cfgblk->pred_ptr[k];
        RPostId pred = node2rpost.ptr[pred_blk];
        if (pred == -1) continue; // freak edge (not reachable in the cfg)
        if (new_idom == -1) new_idom = pred;
        else if (sfidoms[pred] != -1) {
          new_idom = cfg_sfidom_intersect(sfidoms, new_idom, pred);
        }
      }
      if (sfidoms[i] != new_idom) {
        sfidoms[i] = new_idom;
        changed = true;
      }
    }
    if (!changed) break;
  }
  
  NodeId *sfidoms_nodes = malloc(sizeof(int) * cfg->nodes_len);
  for (int i = 0; i < cfg->nodes_len; ++i) sfidoms_nodes[i] = -1;
  for (int i = 0; i < rpost2node.len; ++i) {
    sfidoms_nodes[rpost2node.ptr[i]] = rpost2node.ptr[sfidoms[i]];
  }
  
  free(sfidoms);
  return sfidoms_nodes;
}

#undef NodeId
#undef RPostId

void cfg_dump(char *file, CFG *cfg) {
  FILE *fd = fopen(file, "w");
  fprintf(fd, "Digraph G {\n");
  for (int i = 0; i < cfg->nodes_len; ++i) {
    fprintf(fd, "  Node%i [label=\"<%i>\"];\n", i, i);
  }
  for (int i = 0; i < cfg->nodes_len; ++i) {
    for (int k = 0; k < cfg->nodes_ptr[i].succ_len; ++k) {
      fprintf(fd, "  Node%i -> Node%i;\n", i, cfg->nodes_ptr[i].succ_ptr[k]);
    }
  }
  RPost2Node rpost2node = cfg_get_reverse_postorder(cfg);
  Node2RPost node2rpost = cfg_invert_rpost(cfg, rpost2node);
  int *sfidoms_ptr = cfg_build_sfidom_list(cfg, rpost2node, node2rpost);
  free(rpost2node.ptr);
  free(node2rpost.ptr);
  
  for (int i = 1; i < cfg->nodes_len; ++i) {
    if (sfidoms_ptr[i] == -1) continue;
    fprintf(fd, "  Node%i -> Node%i [constraint=false,color=red,style=dashed];\n", i, sfidoms_ptr[i]);
  }
  free(sfidoms_ptr);
  fprintf(fd, "}\n");
  fclose(fd);
}

void cfg_build(CFG *cfg, UserFunction *uf) {
  // first pass: determine amount of edges
  int num_edges = 0;
  int blocks_len = uf->body.blocks_len;
  int *num_preds = calloc(sizeof(int), blocks_len); // count predecessors per block
  for (int i = 0; i < blocks_len; ++i) {
    InstrBlock *block = &uf->body.blocks_ptr[i];
    Instr *instr = block->instrs_ptr;
    while (instr != block->instrs_ptr_end) {
      if (instr->type == INSTR_BR) {
        num_edges += 1;
        num_preds[((BranchInstr*)instr)->blk] += 1;
      }
      if (instr->type == INSTR_TESTBR) {
        num_edges += 2;
        num_preds[((TestBranchInstr*)instr)->true_blk] += 1;
        num_preds[((TestBranchInstr*)instr)->false_blk] += 1;
      }
      if (instr->type == INSTR_RETURN) {
        num_edges += 0;
      }
      instr = (Instr*)((char*) instr + instr_size(instr));
    }
  }
  // second pass: build cfg
  int *succ_edgelist = malloc(sizeof(int) * num_edges);
  int *succ_edgelist_cursor = succ_edgelist;
  int *pred_edgelist = malloc(sizeof(int) * num_edges);
  int *pred_edgelist_cursor = pred_edgelist;
  int *pred_cursors = calloc(sizeof(int), blocks_len);
  CFGNode *nodes = malloc(sizeof(CFGNode) * blocks_len);
  // preinitialize the pred_ptrs, which aren't explicit in the code
  for (int i = 0; i < blocks_len; ++i) {
    nodes[i].pred_len = num_preds[i];
    nodes[i].pred_ptr = pred_edgelist_cursor;
    pred_edgelist_cursor += num_preds[i];
  }
  assert(pred_edgelist_cursor == pred_edgelist + num_edges);
  free(num_preds);
  
  for (int i = 0; i < blocks_len; ++i) {
    InstrBlock *block = &uf->body.blocks_ptr[i];
    Instr *instr = block->instrs_ptr;
    nodes[i].succ_ptr = succ_edgelist_cursor;
    bool exit_tracked = false;
    while (instr != block->instrs_ptr_end) {
      if (instr->type == INSTR_BR) {
        exit_tracked = true;
        int blk = ((BranchInstr*)instr)->blk;
        nodes[i].succ_len = 1;
        nodes[i].succ_ptr[0] = blk;
        nodes[blk].pred_ptr[pred_cursors[blk]++] = i;
      }
      if (instr->type == INSTR_TESTBR) {
        exit_tracked = true;
        nodes[i].succ_len = 2;
        int true_blk = ((TestBranchInstr*)instr)->true_blk;
        int false_blk = ((TestBranchInstr*)instr)->false_blk;
        nodes[i].succ_ptr[0] = true_blk;
        nodes[i].succ_ptr[1] = false_blk;
        nodes[true_blk].pred_ptr[pred_cursors[true_blk]++] = i;
        nodes[false_blk].pred_ptr[pred_cursors[false_blk]++] = i;
      }
      if (instr->type == INSTR_RETURN) {
        exit_tracked = true;
        nodes[i].succ_len = 0;
      }
      instr = (Instr*)((char*) instr + instr_size(instr));
    }
    if (!exit_tracked) abort();
    succ_edgelist_cursor += nodes[i].succ_len;
  }
  free(pred_cursors);
  assert(succ_edgelist_cursor == succ_edgelist + num_edges);
  cfg->nodes_ptr = nodes;
  cfg->nodes_len = blocks_len;
  cfg->_pred_edgelist_ptr = pred_edgelist;
  cfg->_succ_edgelist_ptr = succ_edgelist;
}

void cfg_destroy(CFG *cfg) {
  free(cfg->_pred_edgelist_ptr);
  free(cfg->_succ_edgelist_ptr);
  free(cfg->nodes_ptr);
}
