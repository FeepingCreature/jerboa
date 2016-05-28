#ifndef VM_INSTR_H
#define VM_INSTR_H

typedef enum {
  INSTR_GET_ROOT,
  INSTR_GET_CONTEXT,
  INSTR_ACCESS,
  INSTR_CALL,
  INSTR_RETURN,
  INSTR_BR,
  INSTR_TESTBR
} InstrType;

typedef struct {
  InstrType type;
} Instr;

typedef struct {
  Instr base;
  int slot;
} GetRootInstr;

typedef struct {
  Instr base;
  int slot;
} GetContextInstr;

typedef struct {
  Instr base;
  int target_slot, obj_slot;
  char *key;
} AccessInstr;

typedef struct {
  Instr base;
  int target_slot, function_slot;
  int *args_ptr; int args_length;
} CallInstr;

typedef struct {
  Instr base;
  int ret_slot;
} ReturnInstr;

typedef struct {
  Instr base;
  int blk;
} BranchInstr;

typedef struct {
  Instr base;
  int test_slot;
  int true_blk, false_blk;
} TestBranchInstr;

typedef struct {
  Instr** instrs_ptr; int instrs_len;
} InstrBlock;

typedef struct {
  InstrBlock* blocks_ptr; int blocks_len;
} FunctionBody;

#endif
