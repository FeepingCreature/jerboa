#ifndef JERBOA_VM_MYJIT_H
#define JERBOA_VM_MYJIT_H

#include <myjit/jitlib.h>

#include "core.h"

typedef struct _JumpEntry JumpEntry;

struct _JumpEntry {
  jit_op *op;
  int block;
  
  JumpEntry *next;
};

typedef struct {
  struct jit *p;
  
  int current_block;
  jit_label **block_label_ptr;
  JumpEntry *unresolved_jumps;
} JitInfo;

void myjit_flatten(UserFunction *vmfun);

/* see https://raw.githubusercontent.com/torvalds/linux/master/tools/perf/Documentation/jitdump-specification.txt */

typedef struct {
  uint32_t magic; // JiTD;
  uint32_t version;
  uint32_t total_size;
  uint32_t elf_mach;
  uint32_t pad1;
  uint32_t pid;
  uint64_t timestamp;
  uint64_t flags;
} jitdump_header;

typedef struct {
  uint32_t id;
  uint32_t total_size;
  uint64_t timestamp;
} jitdump_record_header;

#define JIT_CODE_LOAD 0

typedef struct {
  jitdump_record_header header;
  uint32_t pid;
  uint32_t tid;
  uint64_t vma;
  uint64_t code_addr;
  uint64_t code_size;
  uint64_t code_index;
  /* function name, zero terminated */
  /* native code */
} jitdump_code_load_record;

#endif
