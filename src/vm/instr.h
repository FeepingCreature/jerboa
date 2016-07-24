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
  WriteArg target;
  int value;
} AllocIntObjectInstr;

typedef struct {
  Instr base;
  WriteArg target;
  bool value;
} AllocBoolObjectInstr;

typedef struct {
  Instr base;
  WriteArg target;
  float value;
} AllocFloatObjectInstr;

typedef struct {
  Instr base;
  WriteArg target;
} AllocArrayObjectInstr;

typedef struct {
  Instr base;
  WriteArg target;
  char *value;
} AllocStringObjectInstr;

typedef struct {
  Instr base;
  WriteArg target;
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
  Arg obj, key;
  WriteArg target;
} AccessInstr;

typedef struct {
  Instr base;
  Arg obj, value, key;
  int target_slot /* scratch space for calls */;
  AssignType type;
} AssignInstr;

typedef struct {
  Instr base;
  Arg obj, key; WriteArg target;
} KeyInObjInstr;

typedef struct {
  Instr base;
  Arg obj, proto; WriteArg target;
} InstanceofInstr;

typedef struct {
  Instr base;
  Arg obj, key, constraint;
} SetConstraintInstr;

typedef struct {
  Instr base;
  int size; // faster than recomputing
  CallInfo info;
} CallInstr;

typedef struct {
  Instr base;
  Arg ret;
} ReturnInstr;

typedef struct {
  Instr base;
  int blk;
} BranchInstr;

typedef struct {
  Instr base;
  Arg test;
  int true_blk, false_blk;
} TestBranchInstr;

typedef struct {
  Instr base;
  int block1; Arg arg1;
  int block2; Arg arg2;
  WriteArg target;
} PhiInstr;

typedef struct {
  Instr base;
  int key_slot, key_len; // fallback slot in case we need to call an overload
  char *key_ptr; size_t key_hash;
  Arg obj;
  WriteArg target;
} AccessStringKeyInstr;

typedef struct {
  Instr base;
  char *key;
  Arg obj, value;
  AssignType type;
} AssignStringKeyInstr;

typedef struct {
  Instr base;
  Arg obj, constraint;
  char *key_ptr; int key_len;
} SetConstraintStringKeyInstr;

typedef struct {
  Instr base;
  int target_refslot;
  int obj_slot;
  char *key_ptr; int key_len;
  size_t key_hash;
} DefineRefslotInstr;

typedef struct {
  Instr base;
  Arg source;
  WriteArg target;
  char *opt_info;
} MoveInstr;

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
