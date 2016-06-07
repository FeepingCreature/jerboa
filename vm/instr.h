#ifndef VM_INSTR_H
#define VM_INSTR_H

#include <stdbool.h>

typedef enum {
  INSTR_GET_ROOT,
  INSTR_GET_CONTEXT,
  INSTR_ALLOC_OBJECT,
  INSTR_ALLOC_INT_OBJECT,
  INSTR_ALLOC_FLOAT_OBJECT,
  INSTR_ALLOC_STRING_OBJECT,
  INSTR_ALLOC_CLOSURE_OBJECT,
  INSTR_CLOSE_OBJECT,
  INSTR_ACCESS,
  INSTR_ASSIGN,
  INSTR_ASSIGN_EXISTING, // replace an existing key; error if missing
  INSTR_ASSIGN_SHADOWING, // shadow an existing key; error if missing
  INSTR_CALL,
  INSTR_RETURN,
  INSTR_BR,
  INSTR_TESTBR
} InstrType;

typedef struct {
  InstrType type;
} Instr;

typedef struct {
  Instr** instrs_ptr; int instrs_len;
} InstrBlock;

typedef struct {
  InstrBlock* blocks_ptr; int blocks_len;
} FunctionBody;

typedef struct {
  int arity; // first n slots are reserved for parameters
  int slots;
  char *name;
  bool is_method;
  FunctionBody body;
} UserFunction;

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
  int target_slot, parent_slot;
} AllocObjectInstr;

typedef struct {
  Instr base;
  int target_slot;
  int value;
} AllocIntObjectInstr;

typedef struct {
  Instr base;
  int target_slot;
  float value;
} AllocFloatObjectInstr;

typedef struct {
  Instr base;
  int target_slot;
  char *value;
} AllocStringObjectInstr;

typedef struct {
  Instr base;
  int target_slot, context_slot;
  UserFunction *fn;
} AllocClosureObjectInstr;

typedef struct {
  Instr base;
  int slot;
} CloseObjectInstr;

typedef struct {
  Instr base;
  int target_slot, obj_slot, key_slot;
} AccessInstr;

typedef struct {
  Instr base;
  int obj_slot, value_slot, key_slot;
} AssignInstr;

typedef struct {
  Instr base;
  int obj_slot, value_slot, key_slot;
} AssignExistingInstr;

typedef struct {
  Instr base;
  int obj_slot, value_slot, key_slot;
} AssignShadowingInstr;

typedef struct {
  Instr base;
  int target_slot, function_slot, this_slot;
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

#endif
