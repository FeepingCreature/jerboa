#include "call.h"

#include <stdio.h>

Object *call_function(Object *context, UserFunction *fn, Object **args_ptr, int args_len) {
  int num_slots = fn->slots;
  
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
