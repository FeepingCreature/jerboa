#include "call.h"

#include <stdio.h>
#include "gc.h"

int cyclecount = 0;

Object *call_function(Object *context, UserFunction *fn, Object **args_ptr, int args_len) {
  int num_slots = fn->slots;
  
  Object *root = context;
  while (root->parent) root = root->parent;
  
  Object **slots = calloc(sizeof(Object*), num_slots);
  void *frameroots = gc_add_roots(slots, num_slots);
  
  assert(args_len == fn->arity);
  for (int i = 0; i < args_len; ++i) {
    slots[i] = args_ptr[i];
  }
  
  assert(fn->body.blocks_len > 0);
  InstrBlock *block = fn->body.blocks_ptr;
  int instr_offs = 0;
  while (true) {
    if (!(instr_offs < block->instrs_len)) {
      fprintf(stderr, "Interpreter error: reached end of block without branch instruction! (%li)\n", block - fn->body.blocks_ptr);
      exit(1);
    }
    cyclecount ++;
    Instr *instr = block->instrs_ptr[instr_offs++];
    switch (instr->type) {
      case INSTR_GET_ROOT:{
        GetRootInstr *get_root_instr = (GetRootInstr*) instr;
        int slot = get_root_instr->slot;
        assert(slot < num_slots);
        slots[get_root_instr->slot] = root;
      } break;
      case INSTR_GET_CONTEXT:{
        GetContextInstr *get_context_instr = (GetContextInstr*) instr;
        int slot = get_context_instr->slot;
        assert(slot < num_slots);
        slots[slot] = context;
      } break;
      case INSTR_ACCESS: {
        AccessInstr *access_instr = (AccessInstr*) instr;
        int target_slot = access_instr->target_slot, obj_slot = access_instr->obj_slot;
        int key_slot = access_instr->key_slot;
        
        assert(key_slot < num_slots && slots[key_slot]);
        Object *string_base = object_lookup(root, "string", NULL);
        StringObject *skey = (StringObject*) obj_instance_of(slots[key_slot], string_base);
        assert(skey);
        char *key = skey->value;
        
        assert(obj_slot < num_slots);
        Object *obj = slots[obj_slot];
        
        bool object_found;
        Object *value = object_lookup(obj, key, &object_found);
        if (!object_found) {
          fprintf(stderr, "> identifier not found: '%s'\n", key);
          assert(false);
        }
        
        assert(target_slot < num_slots);
        slots[target_slot] = value;
      } break;
      case INSTR_ASSIGN: {
        AssignInstr *assign_instr = (AssignInstr*) instr;
        int obj_slot = assign_instr->obj_slot, value_slot = assign_instr->value_slot;
        int key_slot = assign_instr->key_slot;
        assert(obj_slot < num_slots);
        assert(value_slot < num_slots);
        assert(key_slot < num_slots && slots[key_slot]);
        Object *string_base = object_lookup(root, "string", NULL);
        StringObject *skey = (StringObject*) obj_instance_of(slots[key_slot], string_base);
        assert(skey);
        char *key = skey->value;
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
        int key_slot = assign_existing_instr->key_slot;
        assert(obj_slot < num_slots);
        assert(value_slot < num_slots);
        assert(key_slot < num_slots && slots[key_slot]);
        Object *string_base = object_lookup(root, "string", NULL);
        StringObject *skey = (StringObject*) obj_instance_of(slots[key_slot], string_base);
        assert(skey);
        char *key = skey->value;
        Object *obj = slots[obj_slot];
        object_set_existing(obj, key, slots[value_slot]);
        // fprintf(stderr, "> obj set '%s'\n", key);
      } break;
      case INSTR_ASSIGN_SHADOWING: {
        AssignShadowingInstr *assign_shadowing_instr = (AssignShadowingInstr*) instr;
        int obj_slot = assign_shadowing_instr->obj_slot, value_slot = assign_shadowing_instr->value_slot;
        int key_slot = assign_shadowing_instr->key_slot;
        assert(obj_slot < num_slots);
        assert(value_slot < num_slots);
        assert(key_slot < num_slots && slots[key_slot]);
        Object *string_base = object_lookup(root, "string", NULL);
        StringObject *skey = (StringObject*) obj_instance_of(slots[key_slot], string_base);
        assert(skey);
        char *key = skey->value;
        Object *obj = slots[obj_slot];
        object_set_shadowing(obj, key, slots[value_slot]);
        // fprintf(stderr, "> obj set '%s'\n", key);
      } break;
      case INSTR_ALLOC_OBJECT:{
        AllocObjectInstr *alloc_obj_instr = (AllocObjectInstr*) instr;
        int target_slot = alloc_obj_instr->target_slot, parent_slot = alloc_obj_instr->parent_slot;
        assert(target_slot < num_slots);
        assert(parent_slot < num_slots);
        Object *parent_obj = slots[parent_slot];
        if (parent_obj) assert(!(parent_obj->flags & OBJ_NOINHERIT));
        Object *obj = alloc_object(root, slots[parent_slot]);
        slots[target_slot] = obj;
      } break;
      case INSTR_ALLOC_INT_OBJECT:{
        AllocIntObjectInstr *alloc_int_obj_instr = (AllocIntObjectInstr*) instr;
        int target_slot = alloc_int_obj_instr->target_slot, value = alloc_int_obj_instr->value;
        assert(target_slot < num_slots);
        Object *obj = alloc_int(context, value);
        slots[target_slot] = obj;
      } break;
      case INSTR_ALLOC_FLOAT_OBJECT:{
        AllocFloatObjectInstr *alloc_float_obj_instr = (AllocFloatObjectInstr*) instr;
        int target_slot = alloc_float_obj_instr->target_slot; float value = alloc_float_obj_instr->value;
        assert(target_slot < num_slots);
        Object *obj = alloc_float(context, value);
        slots[target_slot] = obj;
      } break;
      case INSTR_ALLOC_STRING_OBJECT:{
        AllocStringObjectInstr *alloc_string_obj_instr = (AllocStringObjectInstr*) instr;
        int target_slot = alloc_string_obj_instr->target_slot; char *value = alloc_string_obj_instr->value;
        assert(target_slot < num_slots);
        Object *obj = alloc_string(context, value);
        slots[target_slot] = obj;
      } break;
      case INSTR_ALLOC_CLOSURE_OBJECT:{
        AllocClosureObjectInstr *alloc_closure_obj_instr = (AllocClosureObjectInstr*) instr;
        int target_slot = alloc_closure_obj_instr->target_slot, context_slot = alloc_closure_obj_instr->context_slot;
        assert(target_slot < num_slots);
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
        int this_slot = call_instr->this_slot, args_length = call_instr->args_length;
        assert(target_slot < num_slots);
        assert(function_slot < num_slots);
        assert(this_slot < num_slots);
        Object *this_obj = slots[this_slot];
        Object *fn_obj = slots[function_slot];
        // validate function type
        Object *closure_base = object_lookup(root, "closure", NULL);
        Object *function_base = object_lookup(root, "function", NULL);
        FunctionObject *fn = (FunctionObject*) obj_instance_of(fn_obj, function_base);
        ClosureObject *cl = (ClosureObject*) obj_instance_of(fn_obj, closure_base);
        assert(cl || fn);
        // form args array from slots
        Object **args = malloc(sizeof(Object*) * args_length);
        for (int i = 0; i < args_length; ++i) {
          int argslot = call_instr->args_ptr[i];
          assert(argslot < num_slots);
          args[i] = slots[argslot];
        }
        Object *obj;
        if (cl) obj = cl->base.fn_ptr(context, this_obj, fn_obj, args, args_length);
        else obj = fn->fn_ptr(context, this_obj, fn_obj, args, args_length);
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
        Object *bool_base = object_lookup(root, "bool", NULL);
        Object *int_base = object_lookup(root, "int", NULL);
        Object *b_test_value = obj_instance_of(test_value, bool_base);
        Object *i_test_value = obj_instance_of(test_value, int_base);
        
        bool test = false;
        if (b_test_value) {
          if (((BoolObject*) b_test_value)->value == true) test = true;
        } else if (i_test_value) {
          if (((IntObject*) i_test_value)->value != 0) test = true;
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
  Object *context = alloc_object(fn_obj->context, fn_obj->context);
  object_set(context, "this", thisptr);
  return call_function(context, &fn_obj->vmfun, args_ptr, args_len);
}

Object *alloc_closure_fn(Object *context, UserFunction *fn) {
  Object *root = context;
  while (root->parent) root = root->parent;
  Object *cl_base = object_lookup(root, "closure", NULL);
  ClosureObject *obj = calloc(sizeof(ClosureObject), 1);
  obj->base.base.parent = cl_base;
  if (fn->is_method) obj->base.fn_ptr = method_handler;
  else obj->base.fn_ptr = function_handler;
  obj->context = context;
  obj->vmfun = *fn;
  return (Object*) obj;
}
