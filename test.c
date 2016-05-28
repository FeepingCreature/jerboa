#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <assert.h>

#define alloc(T, ...) ({ T *ptr = malloc(sizeof(T)); *ptr = ((T) __VA_ARGS__); ptr; })

typedef enum {
  OBJ_NONE = 0,
  OBJ_CLOSED = 0x1, // no entries can be added or removed
  OBJ_IMMUTABLE = 0x2 // no entries' values can be changed
} ObjectFlags;

struct _Object;
typedef struct _Object Object;

struct _TableEntry;
typedef struct _TableEntry TableEntry;

struct _Table;
typedef struct _Table Table;

typedef struct _TableEntry {
  char *name;
  Object *value;
  TableEntry *next;
} TableEntry;

typedef struct _Table {
  TableEntry entry;
} Table;

typedef struct _Object {
  Object *parent;
  ObjectFlags flags;
  // int refs;
  Table tbl;
} Object;

Object **table_lookup_ref_alloc(Table *tbl, char *key, TableEntry** first_free_ptr) {
  if (tbl->entry.name == NULL) {
    if (first_free_ptr) *first_free_ptr = &tbl->entry;
    return NULL;
  }
  TableEntry *entry = &tbl->entry, *prev_entry;
  while (entry) {
    if (strcmp(key, entry->name) == 0) return &entry->value;
    prev_entry = entry;
    entry = entry->next;
  }
  if (first_free_ptr) {
    TableEntry *new_entry = calloc(sizeof(TableEntry), 1);
    prev_entry->next = new_entry;
    *first_free_ptr = new_entry;
  }
  return NULL;
}

Object *table_lookup(Table *tbl, char *key) {
  Object **ptr = table_lookup_ref_alloc(tbl, key, NULL);
  if (ptr == NULL) return NULL;
  return *ptr;
}

void table_set(Table *tbl, char *key, Object *value) {
  TableEntry *freeptr;
  Object **ptr = table_lookup_ref_alloc(tbl, key, &freeptr);
  if (ptr == NULL) {
    freeptr->name = key;
    ptr = &freeptr->value;
  }
  *ptr = value;
}

void object_set(Object *obj, char *key, Object *value) {
  assert(obj != NULL);
  Object *current = obj;
  while (current) {
    Object **ptr = table_lookup_ref_alloc(&current->tbl, key, NULL);
    if (ptr != NULL) {
      assert(!(current->flags & OBJ_IMMUTABLE));
      *ptr = value;
      return;
    }
    current = current->parent;
  }
  assert(!(obj->flags & OBJ_CLOSED));
  TableEntry *freeptr;
  Object **ptr = table_lookup_ref_alloc(&obj->tbl, key, &freeptr);
  assert(ptr == NULL);
  freeptr->name = key;
  freeptr->value = value;
}

typedef Object* (*VMFunctionPointer)(Object *context, Object *fn, Object **args_ptr, int args_len);

typedef struct {
  Object base;
  VMFunctionPointer fn_ptr;
} FunctionObject;

typedef struct {
  Object base;
  int value;
} IntObject;

typedef struct {
  Object base;
  bool value;
} BoolObject;

// instructions

/*
 * get_root [var]
 * access [var] = [var] . [key]
 * lookup [var] = [key]
 * call [var] = [function] ( [arg]* )
 * 
 * return [var]
 * br [block]
 * testbr [var] [trueblock] [falseblock]
 */

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

typedef struct {
  int arity; // first n slots are reserved for parameters
  int slots;
  char *name;
  FunctionBody body;
} UserFunction;

typedef struct {
  FunctionObject base;
  UserFunction vmfun;
} UserFunctionObject;

Object *alloc_object(Object *parent) {
  Object *obj = calloc(sizeof(Object), 1);
  obj->parent = parent;
  return obj;
}

Object *alloc_int(Object *context, int value) {
  Object *root = context;
  while (root->parent) root = root->parent;
  Object *int_base = table_lookup(&root->tbl, "int");
  IntObject *obj = calloc(sizeof(IntObject), 1);
  obj->base.parent = int_base;
  obj->value = value;
  return (Object*) obj;
}

Object *alloc_bool(Object *context, int value) {
  Object *root = context;
  while (root->parent) root = root->parent;
  Object *bool_base = table_lookup(&root->tbl, "bool");
  BoolObject *obj = calloc(sizeof(BoolObject), 1);
  obj->base.parent = bool_base;
  obj->value = value;
  return (Object*) obj;
}

Object *alloc_fn(Object *context, VMFunctionPointer fn) {
  Object *root = context;
  while (root->parent) root = root->parent;
  Object *fn_base = table_lookup(&root->tbl, "function");
  FunctionObject *obj = calloc(sizeof(FunctionObject), 1);
  obj->base.parent = fn_base;
  obj->fn_ptr = fn;
  return (Object*) obj;
}

Object *call_function(Object *context, UserFunction *fn, Object **args_ptr, int args_len) {
  int num_slots = fn->arity + fn->slots;
  
  Object **slots = calloc(sizeof(Object*), num_slots);
  
  assert(args_len == fn->arity);
  for (int i = 0; i < args_len; ++i) {
    slots[i] = args_ptr[i];
  }
  
  assert(fn->body.blocks_len > 0);
  InstrBlock *block = &fn->body.blocks_ptr[0];
  int instr_offs = 0;
  while (true) {
    if (!(instr_offs < block->instrs_len)) {
      fprintf(stderr, "Interpreter error: reached end of block without branch instruction!\n");
      exit(1);
    }
    Instr *instr = block->instrs_ptr[instr_offs++];
    switch (instr->type) {
      case INSTR_GET_ROOT:{
        GetRootInstr *get_root_instr = (GetRootInstr*) instr;
        int slot = get_root_instr->slot;
        Object *root = context;
        while (root->parent) root = root->parent;
        assert(slot < num_slots && slots[slot] == NULL);
        slots[get_root_instr->slot] = root;
      } break;
      case INSTR_GET_CONTEXT:{
        GetContextInstr *get_context_instr = (GetContextInstr*) instr;
        int slot = get_context_instr->slot;
        assert(slot < num_slots && slots[slot] == NULL);
        slots[slot] = context;
      } break;
      case INSTR_ACCESS: {
        AccessInstr *access_instr = (AccessInstr*) instr;
        int target_slot = access_instr->target_slot, obj_slot = access_instr->obj_slot;
        char *key = access_instr->key;
        assert(target_slot < num_slots && slots[target_slot] == NULL);
        assert(obj_slot < num_slots);
        Object *obj = slots[obj_slot];
        while (obj) {
          Object *value = table_lookup(&obj->tbl, key);
          if (value) {
            slots[target_slot] = value;
            break;
          }
          obj = obj->parent;
        }
        // missing object/missing key == null
      } break; 
      case INSTR_CALL: {
        CallInstr *call_instr = (CallInstr*) instr;
        int target_slot = call_instr->target_slot, function_slot = call_instr->function_slot;
        int args_length = call_instr->args_length;
        assert(target_slot < num_slots && slots[target_slot] == NULL);
        assert(function_slot < num_slots);
        Object *fn_obj = slots[function_slot];
        Object *root = context;
        while (root->parent) root = root->parent;
        // validate function type
        Object *function_base = table_lookup(&root->tbl, "function");
        Object *fn_type = fn_obj->parent;
        while (fn_type->parent) fn_type = fn_type->parent;
        assert(fn_type == function_base);
        FunctionObject *fn = (FunctionObject*) fn_obj;
        // form args array from slots
        Object **args = malloc(sizeof(Object*) * args_length);
        for (int i = 0; i < args_length; ++i) {
          int argslot = call_instr->args_ptr[i];
          assert(argslot < num_slots);
          args[i] = slots[argslot];
        }
        // and call
        slots[target_slot] = fn->fn_ptr(context, fn_obj, args, args_length);
        free(args);
      } break;
      case INSTR_RETURN: {
        ReturnInstr *ret_instr = (ReturnInstr*) instr;
        int ret_slot = ret_instr->ret_slot;
        assert(ret_slot < num_slots);
        Object *res = slots[ret_slot];
        free(slots);
        return res;
      }
      case INSTR_BR: {
        BranchInstr *br_instr = (BranchInstr*) instr;
        int blk = br_instr->blk;
        assert(blk < fn->body.blocks_len);
        block = &fn->body.blocks_ptr[blk];
        instr_offs = 0;
      } break;
      case INSTR_TESTBR: {
        TestBranchInstr *tbr_instr = (TestBranchInstr*) instr;
        int test_slot = tbr_instr->test_slot;
        int true_blk = tbr_instr->true_blk, false_blk = tbr_instr->false_blk;
        assert(test_slot < num_slots);
        Object *test_value = slots[test_slot];
        
        Object *root = context;
        while (root->parent) root = root->parent;
        Object *bool_base = table_lookup(&root->tbl, "bool");
        Object *int_base = table_lookup(&root->tbl, "int");
        
        int test = 0;
        if (test_value && test_value->parent == bool_base) {
          if (((BoolObject*) test_value)->value == 1) test = 1;
        } else if (test_value && test_value->parent == int_base) {
          if (((IntObject*) test_value)->value != 0) test = 1;
        } else {
          test = test_value != NULL;
        }
        
        int target_blk = test ? true_blk : false_blk;
        block = &fn->body.blocks_ptr[target_blk];
        instr_offs = 0;
      } break;
    }
  }
}

Object *user_function_handler(Object *context, Object *fn, Object **args_ptr, int args_len) {
  UserFunctionObject *fn_obj = (UserFunctionObject*) fn;
  return call_function(context, &fn_obj->vmfun, args_ptr, args_len);
}

Object *alloc_user_fn(Object *context, UserFunction *fn) {
  Object *root = context;
  while (root->parent) root = root->parent;
  Object *fn_base = table_lookup(&root->tbl, "function");
  UserFunctionObject *obj = calloc(sizeof(UserFunctionObject), 1);
  obj->base.base.parent = fn_base;
  obj->base.fn_ptr = user_function_handler;
  obj->vmfun = *fn;
  return (Object*) obj;
}

Object *equals(Object *context, Object *fn, Object **args_ptr, int args_len) {
  assert(args_len == 2);
  Object *root = context;
  while (root->parent) root = root->parent;
  Object *int_base = table_lookup(&root->tbl, "int");
  
  Object *obj1 = args_ptr[0], *obj2 = args_ptr[1];
  if (obj1->parent == int_base && obj2->parent == int_base) {
    int test = ((IntObject*) obj1)->value == ((IntObject*) obj2)->value;
    return alloc_bool(context, test?true:false);
  }
  assert(false);
}

Object *add(Object *context, Object *fn, Object **args_ptr, int args_len) {
  assert(args_len == 2);
  Object *root = context;
  while (root->parent) root = root->parent;
  Object *int_base = table_lookup(&root->tbl, "int");
  
  Object *obj1 = args_ptr[0], *obj2 = args_ptr[1];
  if (obj1->parent == int_base && obj2->parent == int_base) {
    int res = ((IntObject*) obj1)->value + ((IntObject*) obj2)->value;
    return alloc_int(context, res);
  }
  assert(false);
}

Object *sub(Object *context, Object *fn, Object **args_ptr, int args_len) {
  assert(args_len == 2);
  Object *root = context;
  while (root->parent) root = root->parent;
  Object *int_base = table_lookup(&root->tbl, "int");
  
  Object *obj1 = args_ptr[0], *obj2 = args_ptr[1];
  if (obj1->parent == int_base && obj2->parent == int_base) {
    int res = ((IntObject*) obj1)->value - ((IntObject*) obj2)->value;
    return alloc_int(context, res);
  }
  assert(false);
}

Object *mul(Object *context, Object *fn, Object **args_ptr, int args_len) {
  assert(args_len == 2);
  Object *root = context;
  while (root->parent) root = root->parent;
  Object *int_base = table_lookup(&root->tbl, "int");
  
  Object *obj1 = args_ptr[0], *obj2 = args_ptr[1];
  if (obj1->parent == int_base && obj2->parent == int_base) {
    int res = ((IntObject*) obj1)->value * ((IntObject*) obj2)->value;
    return alloc_int(context, res);
  }
  assert(false);
}

int main(int argc, char **argv) {
  Object *root = alloc_object(NULL);
  object_set(root, "int", alloc_object(NULL));
  object_set(root, "bool", alloc_object(NULL));
  object_set(root, "function", alloc_object(NULL));
  object_set(root, "=", alloc_fn(root, equals));
  object_set(root, "+", alloc_fn(root, add));
  object_set(root, "-", alloc_fn(root, sub));
  object_set(root, "*", alloc_fn(root, mul));
  
  Object *const_ints = alloc_object(NULL);
  object_set(const_ints, "int1", alloc_int(root, 1));
  object_set(root, "const_ints", const_ints);
  
  UserFunction *fac_fn = malloc(sizeof(UserFunction));
  fac_fn->arity = 1;
  fac_fn->slots = 15;
  fac_fn->name = "fac";
  fac_fn->body.blocks_len = 3;
  fac_fn->body.blocks_ptr = malloc(sizeof(InstrBlock) * 3);
  InstrBlock *blocks_ptr = fac_fn->body.blocks_ptr;
  blocks_ptr[0].instrs_len = 7;
  blocks_ptr[0].instrs_ptr = malloc(sizeof(Instr*) * 7);
  blocks_ptr[0].instrs_ptr[0] = (Instr*) alloc(GetRootInstr, {{INSTR_GET_ROOT}, 2});
  blocks_ptr[0].instrs_ptr[1] = (Instr*) alloc(AccessInstr, {{INSTR_ACCESS}, 3, 2, "const_ints"});
  blocks_ptr[0].instrs_ptr[2] = (Instr*) alloc(AccessInstr, {{INSTR_ACCESS}, 4, 3, "int1"});
  blocks_ptr[0].instrs_ptr[3] = (Instr*) alloc(GetContextInstr, {{INSTR_GET_CONTEXT}, 5});
  blocks_ptr[0].instrs_ptr[4] = (Instr*) alloc(AccessInstr, {{INSTR_ACCESS}, 6, 5, "="});
  blocks_ptr[0].instrs_ptr[5] = (Instr*) alloc(CallInstr, {{INSTR_CALL}, 7, 6, (int[]) {0, 4}, 2});
  blocks_ptr[0].instrs_ptr[6] = (Instr*) alloc(TestBranchInstr, {{INSTR_TESTBR}, 7, 1, 2});
  blocks_ptr[1].instrs_len = 1;
  blocks_ptr[1].instrs_ptr = malloc(sizeof(Instr*) * 1);
  blocks_ptr[1].instrs_ptr[0] = (Instr*) alloc(ReturnInstr, {{INSTR_RETURN}, 4});
  blocks_ptr[2].instrs_len = 8;
  blocks_ptr[2].instrs_ptr = malloc(sizeof(Instr*) * 8);
  blocks_ptr[2].instrs_ptr[0] = (Instr*) alloc(GetContextInstr, {{INSTR_GET_CONTEXT}, 8});
  blocks_ptr[2].instrs_ptr[1] = (Instr*) alloc(AccessInstr, {{INSTR_ACCESS}, 9, 8, "-"});
  blocks_ptr[2].instrs_ptr[2] = (Instr*) alloc(CallInstr, {{INSTR_CALL}, 10, 9, (int[]) {0, 4}, 2});
  blocks_ptr[2].instrs_ptr[3] = (Instr*) alloc(AccessInstr, {{INSTR_ACCESS}, 11, 8, "fac"});
  blocks_ptr[2].instrs_ptr[4] = (Instr*) alloc(CallInstr, {{INSTR_CALL}, 12, 11, (int[]) {10}, 1});
  blocks_ptr[2].instrs_ptr[5] = (Instr*) alloc(AccessInstr, {{INSTR_ACCESS}, 13, 8, "*"});
  blocks_ptr[2].instrs_ptr[6] = (Instr*) alloc(CallInstr, {{INSTR_CALL}, 14, 13, (int[]) {0, 12}, 2});
  blocks_ptr[2].instrs_ptr[7] = (Instr*) alloc(ReturnInstr, {{INSTR_RETURN}, 14});
  Object *fac = alloc_user_fn(root, fac_fn);
  object_set(root, "fac", fac);
  
  Object **args_ptr = malloc(sizeof(Object*) * 1);
  args_ptr[0] = alloc_int(root, 5);
  Object *res = user_function_handler(root, fac, args_ptr, 1);
  IntObject *res_int = (IntObject*) res;
  printf("fac(5) = %i\n", res_int->value);
  return 0;
}
