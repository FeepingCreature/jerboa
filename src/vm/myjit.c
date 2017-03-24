#include "vm/myjit.h"
#include "object.h"

#include <sys/syscall.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <elf.h>

#define JIT_READ_STRUCT(R, T, M) jit_ldxi_u(p, R, R, offsetof(T, M), sizeof ((T) {0}).M);

#define JIT_WRITE_STRUCT(R, T, M, R2) jit_stxi(p, offsetof(T, M), R, R2, sizeof ((T) {0}).M);

#define JIT_READ_ARRAY(R, T, O) jit_ldxi_u(p, R, R, sizeof(T) * O, sizeof(T));

#define JIT_READ_ARRAY_STRUCT(R, T, O, M) jit_ldxi_u(p, R, R, sizeof(T) * O + offsetof(T, M), sizeof ((T) {0}).M);

#define JIT_READ_ARRAY_THEN_STRUCT(R, T1, O, T2, M) jit_ldxi_u(p, R, R, sizeof(T1) * O + offsetof(T2, M), sizeof ((T2) {0}).M);

#define JIT_READ_ARRAY_THEN_ARRAY(R, T1, O1, T2, O2) jit_ldxi_u(p, R, R, sizeof(T1) * O1 + sizeof(T2) * O2, sizeof(T2));

int jit_dump_index = 0;

static void load_frame_jit(JitInfo *jit, int reg) {
  struct jit * p = jit->p;
  
  jit_getarg(p, reg, 0);
  JIT_READ_STRUCT(reg, VMState, frame);
}

void jit_printf(struct jit *p, const char *msg) {
  jit_prepare(p);
  jit_putargi(p, msg);
  jit_call(p, printf);
}

void jit_printf_r(struct jit *p, const char *msg, int reg1) {
  jit_prepare(p);
  jit_putargi(p, msg);
  jit_putargr(p, reg1);
  jit_call(p, printf);
}

void jit_printf_ir(struct jit *p, const char *msg, int imm1, int reg2) {
  jit_prepare(p);
  jit_putargi(p, msg);
  jit_putargi(p, imm1);
  jit_putargr(p, reg2);
  jit_call(p, printf);
}

void jit_printf_iir(struct jit *p, const char *msg, int imm1, int imm2, int reg3) {
  jit_prepare(p);
  jit_putargi(p, msg);
  jit_putargi(p, imm1);
  jit_putargi(p, imm2);
  jit_putargr(p, reg3);
  jit_call(p, printf);
}

static void load_arg_bool_jit(JitInfo *jit, int reg, Arg test) {
  struct jit * p = jit->p;
  // TODO
  /*if (test.kind == ARG_SLOT) {
    assert(arg.slot < frame->slots_len);
  } else if (kind == ARG_REFSLOT) {
    assert(arg.refslot < frame->refslots_len);
  } else assert(kind == ARG_VALUE);
  */
  
  if (test.kind == ARG_SLOT) {
    load_frame_jit(jit, reg);
    // JIT_READ_STRUCT(reg, Callframe, uf);
    // JIT_READ_STRUCT(reg, UserFunction, slots);
    // jit_printf_ir(p, "JIT slot read %i of %i\n", test.slot, reg);
    
    load_frame_jit(jit, reg);
    assert(test.slot.is_resolved);
    // TODO assert type == TYPE_BOOL
    JIT_READ_ARRAY_THEN_STRUCT(reg, unsigned char, test.slot.offset, Value, b);
  } else if (test.kind == ARG_REFSLOT) {
    load_frame_jit(jit, reg);
    assert(test.refslot.is_resolved);
    // TODO assert type == TYPE_BOOL
    JIT_READ_ARRAY_THEN_ARRAY(reg, unsigned char, test.refslot.offset, TableEntry*, 0);
    JIT_READ_STRUCT(reg, TableEntry, value.b);
  } else {
    jit_movi(p, reg, AS_BOOL(test.value));
  }
}

static void jump_to_block_jit(JitInfo *jit, int block, int T0, int T1, int T2) {
  struct jit * p = jit->p;
  
  // load_frame_jit(jit, T0);
  // JIT_READ_STRUCT(T0, Callframe, uf);
  // JIT_READ_STRUCT(T0, UserFunction, body.blocks_len);
  // jit_printf_ir(p, "JIT seek block %i of %i\n", block, T0);
  
  // state->instr = (Instr*) ((char*) frame->uf->body.instrs_ptr + frame->uf->body.blocks_ptr[target_blk].offset);
  load_frame_jit(jit, T1); // T1 = frame
  
  jit_movr(p, T0, T1); // T0 = frame
  JIT_READ_STRUCT(T1, Callframe, uf); // T1 = uf
  jit_movr(p, T2, T1); // T2 = uf
  JIT_READ_STRUCT(T1, UserFunction, body.blocks_ptr);
  JIT_READ_ARRAY_STRUCT(T1, InstrBlock, block, offset);
  
  // T2 = uf
  JIT_READ_STRUCT(T2, UserFunction, body.instrs_ptr);
  jit_addr(p, T2, T2, T1);
  
  jit_getarg(p, T1, 0); // T1 = state
  JIT_WRITE_STRUCT(T1, VMState, instr, T2);
  
  // frame->prev_block = frame->block;
  jit_movr(p, T1, T0); // T1 = frame
  JIT_READ_STRUCT(T1, Callframe, block);
  JIT_WRITE_STRUCT(T0, Callframe, prev_block, T1);
  
  // frame->block = target_blk;
  
  jit_movi(p, T1, block);
  JIT_WRITE_STRUCT(T0, Callframe, block, T1);
  
  if (block > jit->current_block) {
    jit_op *jmp_op = jit_jmpi(p, JIT_FORWARD);
    JumpEntry *entry = malloc(sizeof(JumpEntry));
    // TODO make double-ended to avoid unconscionable O(n^2) cost
    entry->next = jit->unresolved_jumps;
    entry->block = block;
    entry->op = jmp_op;
    jit->unresolved_jumps = entry;
  } else {
    jit_jmpi(p, jit->block_label_ptr[block]);
  }
}

static void testbr_instr_jit(JitInfo *jit, TestBranchInstr *instr, int T0, int T1, int T2) {
  struct jit * p = jit->p;
  
  load_frame_jit(jit, T0);
  // jit_printf_r(p, "JIT testbr start %p\n", T0);

  int true_blk = instr->true_blk, false_blk = instr->false_blk;
  assert(instr->test.kind != ARG_REFSLOT); // TODO document why
  
  load_arg_bool_jit(jit, T1, instr->test);
  
  jit_op *falsecase = jit_beqi(p, JIT_FORWARD, T1, (jit_value) false);
  // jit_printf(p, "JIT testbr case true\n");
  
  jump_to_block_jit(jit, true_blk, T0, T1, T2);
  
  jit_patch(p, falsecase);
  // jit_printf(p, "JIT testbr case false\n");
  
  jump_to_block_jit(jit, false_blk, T0, T1, T2);
}

static void call_direct_instr_jit(JitInfo *jit, CallFunctionDirectInstr *instr, int T0, int T1) {
  struct jit * p = jit->p;
  CallInfo *info = &instr->info;
  
  jit_getarg(p, T0, 0); // state
  
  if (instr->fast) {
    jit_prepare(p);
    jit_putargr(p, T0);
    jit_call(p, instr->base.fn);
  } else {
    jit_prepare(p);
    jit_putargr(p, T0);
    jit_putargi(p, info);
    jit_call(p, instr->fn);
  }
  
  // read state->runstate
  jit_getarg(p, T0, 0);
  JIT_READ_STRUCT(T0, VMState, runstate);
  
  // bail on fail
  jit_op *succeeded = jit_beqi(p, JIT_FORWARD, T0, (jit_value) VM_RUNNING);
  // failed
  jit_reti(p, vm_halt);
  
  jit_patch(p, succeeded);
  
  Instr *next_instr = (Instr*) ((char*) instr + instr->size);
  jit_movi(p, T1, next_instr);
  jit_getarg(p, T0, 0);
  JIT_WRITE_STRUCT(T0, VMState, instr, T1);
}

static void patch_jit_labels(JitInfo *jit, int block) {
  JumpEntry **curp = &jit->unresolved_jumps;
  while (*curp) {
    if ((*curp)->block == block) {
      jit_patch(jit->p, (*curp)->op);
      *curp = (*curp)->next;
    } else {
      curp = &(*curp)->next;
    }
  }
}

#if defined(_i386__) || defined(__x86_64__)
#define USE_NATIVE_TIMESTAMP
#endif
#define JITDUMP_FLAGS_ARCH_TIMESTAMP 1

static uint64_t get_timestamp(void) {
#ifdef USE_NATIVE_TIMESTAMP
  unsigned int low, high;

  __asm__ volatile("rdtsc" : "=a" (low), "=d" (high));

  return low | ((uint64_t)high) << 32;
#else
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) == -1) {
    fprintf(stderr, "clock_gettime failed: %s\n", strerror(errno));
    abort();
  }
  
  return ((uint64_t) ts.tv_sec * 1000000000) + ts.tv_nsec;
#endif
}

void myjit_flatten(UserFunction *vmfun) {
  assert(vmfun->opt_jit_fn == NULL);
  assert(vmfun->proposed_jit_fn == NULL);
  
  struct jit *p = jit_init();
  jit_enable_optimization(p, JIT_OPT_ALL);
  
  JitInfo jit = {0};
  jit.p = p;
  jit.block_label_ptr = malloc(sizeof(JumpEntry) * vmfun->body.blocks_len);
  jit.unresolved_jumps = NULL;
  
  jit_label *start_label = jit_get_label(p);
  jit_prolog(p, &vmfun->proposed_jit_fn);
  jit_declare_arg(p, JIT_UNSIGNED_NUM, sizeof(VMState*));
  
  jit_getarg(p, R(0), 0);
  
  // jit_printf_r(p, "JIT call start %p\n", R(0)); 
  
  jit_op **bail_ptr = NULL;
  int bail_len = 0;
  
  for (int i = 0; i < vmfun->body.blocks_len; ++i) {
    patch_jit_labels(&jit, i);
    jit_label *blk_label = jit_get_label(p);
    jit.current_block = i;
    jit.block_label_ptr[i] = blk_label;
    
    Instr *instr_cur = BLOCK_START(vmfun, i), *instr_end = BLOCK_END(vmfun, i);
    int k = 0; (void) k;
    
    while (instr_cur != instr_end) {
      Instr *instr_next = (Instr*) ((char*) instr_cur + instr_size(instr_cur));
      
      VMInstrFn vm_fn = instr_cur->fn;
      
      // jit_getarg(p, R(2), 0);
      // JIT_READ_STRUCT(R(2), VMState, instr);
      // jit_printf_iir(p, "JIT blk %i instr %i or %p\n", i, k++, R(2));
      
      if (instr_cur->type == INSTR_TESTBR) {
        testbr_instr_jit(&jit, (TestBranchInstr*) instr_cur, R(1), R(2), R(3));
      } else if (instr_cur->type == INSTR_CALL_FUNCTION_DIRECT) {
        call_direct_instr_jit(&jit, (CallFunctionDirectInstr*) instr_cur, R(1), R(2));
      } else if (instr_cur->type == INSTR_RETURN) {
        // regular call that exits vm function
        // TODO inline?
        jit_prepare(p);
        jit_putargr(p, R(0));
        jit_call(p, vm_fn);
        
        JIT_READ_STRUCT(R(0), VMState, instr);
        JIT_READ_STRUCT(R(0), Instr, fn);
        jit_retr(p, R(0)); // return
      } else {
        jit_prepare(p);
        jit_putargr(p, R(0));
        jit_call(p, vm_fn);
        
        jit_ldxi_u(p, R(1), R(0), offsetof(VMState, instr), sizeof(Instr*));
        jit_op *bail = jit_bnei(p, (jit_value) JIT_FORWARD, R(1), (size_t) instr_next);
        bail_ptr = realloc(bail_ptr, ++bail_len * sizeof(jit_op*));
        bail_ptr[bail_len - 1] = bail;
      }
      
      instr_cur = instr_next;
    }
  }
  JIT_READ_STRUCT(R(0), VMState, instr);
  JIT_READ_STRUCT(R(0), Instr, fn);
  jit_retr(p, R(0)); // exit
  
  for (int i = 0; i < bail_len; i++) {
    jit_patch(p, bail_ptr[i]);
  }
  
  // jit_printf_r(p, "JIT bail! %p\n", R(2));
  JIT_READ_STRUCT(R(0), VMState, instr);
  JIT_READ_STRUCT(R(0), Instr, fn);
  jit_retr(p, R(0)); // abort
  
  jit_label *end_label = jit_get_label(p);

  jit_check_code(p, JIT_WARN_ALL);
  
  jit_generate_code(p);
  size_t fnsize = end_label->pos - start_label->pos;
  
  // see https://github.com/torvalds/linux/blob/master/tools/perf/jvmti/jvmti_agent.c
  char *basepath = getenv("JITDUMPDIR");
  if (basepath == NULL) basepath = getenv("HOME");
  if (basepath == NULL) basepath = getenv(".");
  char *dump_path = my_asprintf("%s/.debug/", basepath);
  if (mkdir(dump_path, S_IRWXU) < 0 && errno != EEXIST) {
    fprintf(stderr, "failed to create .debug dir!\n");
    abort();
  }
  dump_path = my_asprintf("%s/.debug/jit/", basepath);
  if (mkdir(dump_path, S_IRWXU) < 0 && errno != EEXIST) {
    fprintf(stderr, "failed to create .debug/jit dir!\n");
    abort();
  }
  
  char *filename = my_asprintf("%s/.debug/jit/jit-%d.dump", basepath, getpid());
  
  bool appending;
  FILE *jitdump;
  if (access(filename, F_OK) != -1) {
    jitdump = fopen(filename, "a");
    appending = true;
  } else {
    jitdump = fopen(filename, "w");
    appending = false;
  }
  if (jitdump == NULL) {
    fprintf(stderr, "cannot open jit dump: %s\n", strerror(errno));
    abort();
  }
  
#ifdef __i386__
#define MACHINE EM_386
#endif
#ifdef __x86_64__
#define MACHINE EM_X86_64
#endif
  if (!appending) {
    jitdump_header jheader = {
      .magic = 0x4A695444, /* JITHEADER_MAGIC */
      .version = 2,
      .total_size = sizeof(jitdump_header),
      .elf_mach = MACHINE,
      .pid = getpid(),
      .timestamp = get_timestamp(),
      .flags = 0
    };
#ifdef USE_NATIVE_TIMESTAMP
    jheader.flags |= JITDUMP_FLAGS_ARCH_TIMESTAMP;
#endif
    fwrite(&jheader, sizeof(jheader), 1, jitdump);
  }
  
  char *fun_name = vmfun->name;
  if (!fun_name) {
    fun_name = my_asprintf("<%p>", vmfun->proposed_jit_fn);
  }
  
  int namelen = strlen(fun_name);
  
  jitdump_code_load_record jloadrecord = {
    .header = {
      .id = JIT_CODE_LOAD,
      .total_size = sizeof(jitdump_code_load_record) + namelen + 1 + fnsize,
      .timestamp = get_timestamp()
    },
    .pid = getpid(),
    .tid = syscall(SYS_gettid),
    .vma = *(size_t*) &vmfun->proposed_jit_fn,
    .code_addr = *(size_t*) &vmfun->proposed_jit_fn,
    .code_size = fnsize,
    .code_index = jit_dump_index++
  };
  fwrite(&jloadrecord, sizeof(jloadrecord), 1, jitdump);
  fwrite(fun_name, namelen + 1, 1, jitdump);
  fwrite(*(void**) &vmfun->proposed_jit_fn, fnsize, 1, jitdump);
  fclose(jitdump);
  
  // place perf hint
  int fd = open(filename, 0);
  int fd_size = lseek(fd, 0, SEEK_END);
  void *map = mmap(NULL, fd_size, PROT_READ|PROT_EXEC, MAP_PRIVATE, fd, 0);
  if (map == MAP_FAILED) {
    fprintf(stderr, "error mmapping perf dump: %s\n", strerror(errno));
    abort();
  }
  munmap(map, 1);
  close(fd);
  
	// printf("generated function at %p: size %li\n", *(void**) &vmfun->proposed_jit_fn, fnsize);
  
  // printf("-- Compilable:\n");
  // jit_dump_ops(p, JIT_DEBUG_COMPILABLE);

  // printf("-- Code:\n");
  // jit_dump_ops(p, JIT_DEBUG_CODE);
  
  // jit_free(p);
  vmfun->opt_jit_fn = vmfun->proposed_jit_fn;
}
