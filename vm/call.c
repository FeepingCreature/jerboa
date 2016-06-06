#include "call.h"

#include <stdio.h>
#include "gc.h"

int cyclecount = 0;

Object *call_function(Object *context, UserFunction *fn, Object **args_ptr, int args_len) {
  int num_slots = fn->slots;
  
  Object **slots = calloc(sizeof(Object*), num_slots);
  void *frameroots = gc_add_roots(slots, num_slots);
  
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
    cyclecount ++;
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
        
        assert(obj_slot < num_slots);
        Object *obj = slots[obj_slot];
        
        Object *value = object_lookup(obj, key);
        if (value == NULL) {
          fprintf(stderr, "> lookup yielded null: '%s'\n", key);
          assert(false);
        }
        
        assert(target_slot < num_slots && slots[target_slot] == NULL);
        slots[target_slot] = value;
      } break;
      case INSTR_ASSIGN: {
        AssignInstr *assign_instr = (AssignInstr*) instr;
        int obj_slot = assign_instr->obj_slot, value_slot = assign_instr->value_slot;
        char *key = assign_instr->key;
        assert(obj_slot < num_slots);
        assert(value_slot < num_slots);
        Object *obj = slots[obj_slot];
        if (obj == NULL) {
          fprintf(stderr, "> assignment to null object");
          assert(false);
        }
        object_set(obj, key, slots[value_slot]);
        // fprintf(stderr, "> obj set '%s'\n", key);
      } break;
      case INSTR_ASSIGN_EXISTING: {
        AssignExistingInstr *assign_existing_instr = (AssignExistingInstr*) instr;
        int obj_slot = assign_existing_instr->obj_slot, value_slot = assign_existing_instr->value_slot;
        char *key = assign_existing_instr->key;
        assert(obj_slot < num_slots);
        assert(value_slot < num_slots);
        Object *obj = slots[obj_slot];
        object_set_existing(obj, key, slots[value_slot]);
        // fprintf(stderr, "> obj set '%s'\n", key);
      } break;
      case INSTR_ALLOC_OBJECT:{
        AllocObjectInstr *alloc_obj_instr = (AllocObjectInstr*) instr;
        int target_slot = alloc_obj_instr->target_slot, parent_slot = alloc_obj_instr->parent_slot;
        assert(target_slot < num_slots && slots[target_slot] == NULL);
        assert(parent_slot < num_slots);
        Object *obj = alloc_object(slots[parent_slot]);
        slots[target_slot] = obj;
      } break;
      case INSTR_ALLOC_INT_OBJECT:{
        AllocIntObjectInstr *alloc_int_obj_instr = (AllocIntObjectInstr*) instr;
        int target_slot = alloc_int_obj_instr->target_slot, value = alloc_int_obj_instr->value;
        assert(target_slot < num_slots && slots[target_slot] == NULL);
        Object *obj = alloc_int(context, value);
        slots[target_slot] = obj;
      } break;
      case INSTR_ALLOC_FLOAT_OBJECT:{
        AllocFloatObjectInstr *alloc_float_obj_instr = (AllocFloatObjectInstr*) instr;
        int target_slot = alloc_float_obj_instr->target_slot; float value = alloc_float_obj_instr->value;
        assert(target_slot < num_slots && slots[target_slot] == NULL);
        Object *obj = alloc_float(context, value);
        slots[target_slot] = obj;
      } break;
      case INSTR_ALLOC_STRING_OBJECT:{
        AllocStringObjectInstr *alloc_string_obj_instr = (AllocStringObjectInstr*) instr;
        int target_slot = alloc_string_obj_instr->target_slot; char *value = alloc_string_obj_instr->value;
        assert(target_slot < num_slots && slots[target_slot] == NULL);
        Object *obj = alloc_string(context, value);
        slots[target_slot] = obj;
      } break;
      case INSTR_ALLOC_CLOSURE_OBJECT:{
        AllocClosureObjectInstr *alloc_closure_obj_instr = (AllocClosureObjectInstr*) instr;
        int target_slot = alloc_closure_obj_instr->target_slot, context_slot = alloc_closure_obj_instr->context_slot;
        assert(target_slot < num_slots && slots[target_slot] == NULL);
        assert(context_slot < num_slots);
        Object *obj = alloc_closure_fn(slots[context_slot], alloc_closure_obj_instr->fn);
        slots[target_slot] = obj;
      } break;
      case INSTR_CLOSE_OBJECT:{
        CloseObjectInstr *close_object_instr = (CloseObjectInstr*) instr;
        int slot = close_object_instr->slot;
        assert(slot < num_slots);
        Object *obj = slots[slot];
        assert(!(obj->flags & OBJ_CLOSED));
        obj->flags |= OBJ_CLOSED;
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
        Object *function_base = object_lookup(root, "function");
        Object *closure_base = object_lookup(root, "closure");
        Object *fn_type = fn_obj->parent;
        while (fn_type->parent) fn_type = fn_type->parent;
        assert(fn_type == function_base || fn_type == closure_base);
        FunctionObject *fn = (FunctionObject*) fn_obj;
        // form args array from slots
        Object **args = malloc(sizeof(Object*) * args_length);
        for (int i = 0; i < args_length; ++i) {
          int argslot = call_instr->args_ptr[i];
          assert(argslot < num_slots);
          args[i] = slots[argslot];
        }
        // and call
        Object *obj = fn->fn_ptr(context, NULL, fn_obj, args, args_length);
        slots[target_slot] = obj;
        free(args);
      } break;
      case INSTR_RETURN: {
        ReturnInstr *ret_instr = (ReturnInstr*) instr;
        int ret_slot = ret_instr->ret_slot;
        assert(ret_slot < num_slots);
        Object *res = slots[ret_slot];
        gc_remove_roots(frameroots);
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
        Object *bool_base = object_lookup(root, "bool");
        Object *int_base = object_lookup(root, "int");
        
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
      default: assert(false); break;
    }
  }
}

Object *function_handler(Object *calling_context, Object *thisptr, Object *fn, Object **args_ptr, int args_len) {
  // discard calling context (lexical scoping!)
  ClosureObject *fn_obj = (ClosureObject*) fn;
  return call_function(fn_obj->context, &fn_obj->vmfun, args_ptr, args_len);
}

Object *method_handler(Object *calling_context, Object *thisptr, Object *fn, Object **args_ptr, int args_len) {
  // discard calling context (lexical scoping!)
  ClosureObject *fn_obj = (ClosureObject*) fn;
  Object *context = alloc_object(fn_obj->context);
  object_set(context, "this", thisptr);
  return call_function(context, &fn_obj->vmfun, args_ptr, args_len);
}

Object *alloc_closure_fn(Object *context, UserFunction *fn) {
  Object *root = context;
  while (root->parent) root = root->parent;
  Object *cl_base = object_lookup(root, "closure");
  ClosureObject *obj = calloc(sizeof(ClosureObject), 1);
  obj->base.base.parent = cl_base;
  obj->base.fn_ptr = function_handler;
  obj->context = context;
  obj->vmfun = *fn;
  return (Object*) obj;
}
