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

struct _Object;
typedef struct _Object Object;

typedef struct {
  Instr base;
  int target_slot, parent_slot;
  bool alloc_stack;
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
  int obj_slot;
  bool on_stack;
} FreeObjectInstr;

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
  Arg obj1, obj2;
  WriteArg target;
} IdenticalInstr;

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
  Arg value;
  WriteArg target;
} TestInstr;

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
  Arg test; // MUST NOT be ref! so that our stackframe
            // can be safely freed before this instr
            // (relevant for testbranch at the end of a loop)
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
  int key_slot; // fallback slot in case we need to call an overload
  FastKey key;
  Arg obj;
  WriteArg target;
} AccessStringKeyInstr;

typedef struct {
  Instr base;
  FastKey key;
  Arg obj, value;
  int target_slot /* scratch space for calls */;
  AssignType type;
} AssignStringKeyInstr;

typedef struct {
  Instr base;
  FastKey key;
  Arg obj; WriteArg target;
} StringKeyInObjInstr;

typedef struct {
  Instr base;
  Arg obj, constraint;
  char *key_ptr; int key_len;
} SetConstraintStringKeyInstr;

typedef struct {
  Instr base;
  int target_refslot;
  int obj_slot;
  FastKey key;
} DefineRefslotInstr;

typedef struct {
  Instr base;
  Arg source;
  WriteArg target;
  char *opt_info;
} MoveInstr;

typedef struct {
  int offset;
  const char *key;
  Object *constraint;
  int slot;
  int refslot;
} StaticFieldInfo;

// object is allocated, some fields are defined, object is closed, and refslots are created for its fields
typedef struct {
  Instr base;
  int target_slot, parent_slot;
  bool alloc_stack;
  
  // entries_stored is len of ASOI_INFO
  HashTable tbl;
} AllocStaticObjectInstr;

#define ASOI_INFO(I) ((StaticFieldInfo*)((AllocStaticObjectInstr*)(I) + 1))

typedef FnWrap (*InstrDispatchFn)(Instr*);

typedef struct {
  Instr base;
  int size; // faster than recomputing
  bool fast; // fn returned by dispatch takes care of vmstate management; behaves like a vm instr function
  union {
    VMFunctionPointer fn;
    InstrDispatchFn dispatch_fn;
  };
  CallInfo info; // must be last! has tail!
} CallFunctionDirectInstr;

#endif
