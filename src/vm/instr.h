#ifndef VM_INSTR_H
#define VM_INSTR_H

#include "core.h"

#include <stdbool.h>
#include <stdlib.h>

typedef enum {
  // just write the key to the top object, already-present or not
  // ie. hashtable-type access
  ASSIGN_PLAIN,
  // replace an existing key (somewhere in the chain), error if absent
  // ie. scope-type access
  ASSIGN_EXISTING,
  // shadow an existing key (write to top object), but error if absent anywhere
  // ie. object-type access
  ASSIGN_SHADOWING
} AssignType;

int instr_size(Instr*);

#define BLOCK_START(FN, IDX) ((Instr*) ((char*) (FN)->body.instrs_ptr + (FN)->body.blocks_ptr[IDX].offset))
#define BLOCK_END(FN, IDX) ((Instr*) ((char*) (FN)->body.instrs_ptr + (FN)->body.blocks_ptr[IDX].offset + (FN)->body.blocks_ptr[IDX].size))

typedef enum {
  ARG_SLOT,
  ARG_REFSLOT,
  ARG_VALUE
} ArgKind;

typedef struct {
  ArgKind kind;
  union {
    int slot;
    int refslot;
    Value value;
  };
} Arg;

static inline Value load_arg(VMState *state, Arg arg) {
  if (arg.kind == ARG_SLOT) {
    assert(arg.slot < state->frame->slots_len);
    return state->frame->slots_ptr[arg.slot];
  }
  if (arg.kind == ARG_REFSLOT) {
    assert(arg.slot < state->frame->refslots_len);
    return *state->frame->refslots_ptr[arg.refslot];
  }
  assert(arg.kind == ARG_VALUE);
  return arg.value;
}

char *get_arg_info(Arg arg);

typedef struct {
  Instr base;
  int slot;
} GetRootInstr;

struct _Object;
typedef struct _Object Object;

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
  bool value;
} AllocBoolObjectInstr;

typedef struct {
  Instr base;
  int target_slot;
  float value;
} AllocFloatObjectInstr;

typedef struct {
  Instr base;
  int target_slot;
} AllocArrayObjectInstr;

typedef struct {
  Instr base;
  int target_slot;
  char *value;
} AllocStringObjectInstr;

typedef struct {
  Instr base;
  int target_slot;
  UserFunction *fn;
} AllocClosureObjectInstr;

typedef struct {
  Instr base;
  int slot;
} CloseObjectInstr;

typedef struct {
  Instr base;
  int slot;
} FreezeObjectInstr;

typedef struct {
  Instr base;
  int obj_slot, key_slot;
  int target_slot;
} AccessInstr;

typedef struct {
  Instr base;
  int obj_slot, value_slot, key_slot, target_slot /* scratch space for calls */;
  AssignType type;
} AssignInstr;

typedef struct {
  Instr base;
  int obj_slot, key_slot, target_slot;
} KeyInObjInstr;

typedef struct {
  Instr base;
  int obj_slot, proto_slot, target_slot;
} InstanceofInstr;

typedef struct {
  Instr base;
  int obj_slot, key_slot, constraint_slot;
} SetConstraintInstr;

typedef struct {
  Instr base;
  Arg function;
  int this_slot;
  int args_length; // attached to callinstr as a tail
  int target_slot;
} CallInstr;

typedef struct {
  Instr base;
  int ret_slot;
} ReturnInstr;

typedef struct {
  Instr base;
  int target_slot;
} SaveResultInstr;

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
  Instr base;
  int block1, slot1;
  int block2, slot2;
  int target_slot;
} PhiInstr;

typedef struct {
  Instr base;
  int obj_slot, key_slot; // fallback slot in case we need to call an overload
  char *key_ptr; int key_len;
  size_t key_hash;
  int target_slot;
} AccessStringKeyInstr;

typedef struct {
  Instr base;
  int obj_slot, value_slot; char *key;
  AssignType type;
} AssignStringKeyInstr;

typedef struct {
  Instr base;
  int obj_slot, constraint_slot;
  char *key_ptr; int key_len;
} SetConstraintStringKeyInstr;

typedef struct {
  Instr base;
  int target_slot;
  Value value;
  char *opt_info;
} SetSlotInstr;

typedef struct {
  Instr base;
  int target_refslot;
  int obj_slot;
  char *key_ptr; int key_len;
  size_t key_hash;
} DefineRefslotInstr;

typedef struct {
  Instr base;
  int source_refslot, target_slot;
  char *opt_info;
} ReadRefslotInstr;

typedef struct {
  Instr base;
  int source_slot, target_refslot;
  char *opt_info;
} WriteRefslotInstr;

typedef struct {
  char *name_ptr;
  int name_len;
  size_t name_hash;
  int tbl_offset;
  
  Object *constraint;
  
  int slot;
  int refslot;
} StaticFieldInfo;

// object is allocated, some fields are defined, object is closed, and refslots are created for its fields
typedef struct {
  Instr base;
  int target_slot, parent_slot;
  
  int info_len;
  StaticFieldInfo *info_ptr;
} AllocStaticObjectInstr;

#endif
