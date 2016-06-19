#define _GNU_SOURCE
#include "call.h"

#include <stdio.h>
#include <stdarg.h>
#include "gc.h"
#include "dump.h"

int cyclecount = 0;

#define UNLIKELY(X) __builtin_expect(X, 0)

Callframe *vm_alloc_frame(VMState *state, int slots) {
  // okay. this might get a bit complicated.
  void *ptr = state->stack_ptr;
  if (!ptr) {
    VM_ASSERT(state->stack_len == 0, "internal error") NULL;
    int initial_capacity = sizeof(Callframe);
    ptr = malloc(initial_capacity + sizeof(Callframe));
    ptr = (Callframe*)ptr + 1;
    *((int*)ptr - 1) = initial_capacity;
    state->stack_ptr = ptr;
  } else {
    int capacity = *((int*)ptr - 1);
    int new_size = sizeof(Callframe) * (state->stack_len + 1);
    if (new_size > capacity) {
      int new_capacity = capacity * 2;
      if (new_size > new_capacity) new_capacity = new_size;
      void *old_ptr = ptr, *old_ptr_base = (Callframe*)ptr - 1;
      ptr = malloc(new_capacity + sizeof(Callframe));
      ptr = (Callframe*)ptr + 1;
      *((int*)ptr - 1) = new_capacity;
      // tear down old gc roots
      for (int i = 0; i < state->stack_len; ++i) {
        gc_remove_roots(state, &state->stack_ptr[i].frameroot);
      }
      memcpy(ptr, old_ptr, capacity);
      state->stack_ptr = ptr;
      // add new gc roots
      for (int i = 0; i < state->stack_len; ++i) {
        Callframe *cf = &state->stack_ptr[i];
        gc_add_roots(state, cf->slots_ptr, cf->slots_len, &cf->frameroot);
      }
      free(old_ptr_base);
    }
  }
  
  state->stack_len = state->stack_len + 1;
  Callframe *cf = &state->stack_ptr[state->stack_len - 1];
  cf->slots_len = slots;
  cf->slots_ptr = calloc(sizeof(Object*), cf->slots_len);
  return cf;
}

void vm_error(VMState *state, char *fmt, ...) {
  assert(state->runstate == VM_RUNNING);
  char *errorstr;
  va_list ap;
  va_start(ap, fmt);
  if (-1 == vasprintf(&errorstr, fmt, ap)) abort();
  va_end(ap);
  state->runstate = VM_ERRORED;
  state->error = errorstr;
}

void vm_remove_frame(VMState *state) {
  Callframe *cf = &state->stack_ptr[state->stack_len - 1];
  free(cf->slots_ptr);
  // TODO shrink memory?
  state->stack_len = state->stack_len - 1;
}

static void vm_step(VMState *state, void **args_prealloc) {
  Object *root = state->root;
  Callframe *cf = &state->stack_ptr[state->stack_len - 1];
  
#ifndef NDEBUG
  if (UNLIKELY(!(cf->instr_offs < cf->block->instrs_len))) {
    fprintf(stderr, "Interpreter error: reached end of block without branch instruction! (%li)\n", cf->block - cf->uf->body.blocks_ptr);
    exit(1);
  }
#endif
  
  cyclecount ++;
  Instr *instr = cf->block->instrs_ptr[cf->instr_offs];
  switch (instr->type) {
    case INSTR_GET_ROOT:{
      GetRootInstr *get_root_instr = (GetRootInstr*) instr;
      int slot = get_root_instr->slot;
      VM_ASSERT(slot < cf->slots_len, "internal slot error");
      cf->slots_ptr[get_root_instr->slot] = root;
    } break;
    case INSTR_GET_CONTEXT:{
      GetContextInstr *get_context_instr = (GetContextInstr*) instr;
      int slot = get_context_instr->slot;
      VM_ASSERT(slot < cf->slots_len, "internal slot error");
      cf->slots_ptr[slot] = cf->context;
    } break;
    case INSTR_ACCESS: case INSTR_ACCESS_STRING_KEY: {
      AccessInstr *access_instr = (AccessInstr*) instr;
      AccessStringKeyInstr *aski = (AccessStringKeyInstr*) instr;
      int obj_slot, target_slot;
      if (instr->type == INSTR_ACCESS) {
        obj_slot = access_instr->obj_slot;
        target_slot = access_instr->target_slot;
      } else {
        obj_slot = aski->obj_slot;
        target_slot = aski->target_slot;
      }
      
      VM_ASSERT(obj_slot < cf->slots_len, "internal slot error");
      VM_ASSERT(target_slot < cf->slots_len, "internal slot error");
      Object *obj = cf->slots_ptr[obj_slot];
      
      char *key;
      bool has_char_key = false;
      
      if (instr->type == INSTR_ACCESS) {
        int key_slot = access_instr->key_slot;
        VM_ASSERT(key_slot < cf->slots_len, "internal slot error");
        VM_ASSERT(cf->slots_ptr[key_slot], "key slot null"); // TODO "slot_assigned"
        Object *string_base = object_lookup(root, "string", NULL);
        Object *key_obj = cf->slots_ptr[key_slot];
        VM_ASSERT(key_obj, "key is null");
        StringObject *skey = (StringObject*) obj_instance_of(key_obj, string_base);
        if (skey) {
          key = skey->value;
          has_char_key = true;
        }
      } else {
        key = aski->key;
        has_char_key = true;
      }
      bool object_found = false;
      if (has_char_key) {
        // fprintf(stderr, "> obj get %p . '%s'\n", (void*) obj, key);
        cf->slots_ptr[target_slot] = object_lookup(obj, key, &object_found);
      }
      if (!object_found) {
        Object *index_op = object_lookup(obj, "[]", NULL);
        if (index_op) {
          Object *function_base = object_lookup(root, "function", NULL);
          Object *closure_base = object_lookup(root, "closure", NULL);
          FunctionObject *fn_index_op = (FunctionObject*) obj_instance_of(index_op, function_base);
          ClosureObject *cl_index_op = (ClosureObject*) obj_instance_of(index_op, closure_base);
          VM_ASSERT(fn_index_op || cl_index_op, "index op is neither function nor closure");
          Object *key_obj;
          if (instr->type == INSTR_ACCESS) {
            key_obj = cf->slots_ptr[access_instr->key_slot];
          } else {
            key_obj = alloc_string(state, aski->key);
          }
          
          VMState substate = {0};
          substate.root = state->root;
          substate.gcstate = state->gcstate;
          
          if (fn_index_op) fn_index_op->fn_ptr(&substate, obj, index_op, &key_obj, 1);
          else cl_index_op->base.fn_ptr(&substate, obj, index_op, &key_obj, 1);
          
          vm_run(&substate);
          VM_ASSERT(substate.runstate != VM_ERRORED, "[] overload failed: %s\n", substate.error);
          
          cf->slots_ptr[target_slot] = substate.result_value;
          
          object_found = true; // rely on the [] call to error on its own, if key not found
        }
      }
      if (!object_found) {
        if (has_char_key) {
          VM_ASSERT(false, "property not found: '%s'", key);
        } else {
          VM_ASSERT(false, "property not found!");
        }
      }
    } break;
    case INSTR_ASSIGN: case INSTR_ASSIGN_STRING_KEY: {
      AssignInstr *assign_instr = (AssignInstr*) instr;
      AssignStringKeyInstr *aski = (AssignStringKeyInstr*) instr;
      AssignType assign_type;
      Object *obj, *value_obj;
      char *key;
      bool has_char_key = false;
      if (instr->type == INSTR_ASSIGN) {
        int obj_slot = assign_instr->obj_slot, value_slot = assign_instr->value_slot;
        int key_slot = assign_instr->key_slot;
        VM_ASSERT(obj_slot < cf->slots_len, "slot numbering error");
        VM_ASSERT(value_slot < cf->slots_len, "slot numbering error");
        VM_ASSERT(key_slot < cf->slots_len, "slot numbering error");
        VM_ASSERT(cf->slots_ptr[key_slot], "key slot null"); // TODO see above
        obj = cf->slots_ptr[obj_slot];
        value_obj = cf->slots_ptr[value_slot];
        Object *string_base = object_lookup(root, "string", NULL);
        Object *key_obj = cf->slots_ptr[key_slot];
        StringObject *skey = (StringObject*) obj_instance_of(key_obj, string_base);
        if (skey) {
          key = skey->value;
          assign_type = assign_instr->type;
          has_char_key = true;
        }
      } else {
        int obj_slot = aski->obj_slot, value_slot = aski->value_slot;
        VM_ASSERT(obj_slot < cf->slots_len, "slot numbering error");
        VM_ASSERT(value_slot < cf->slots_len, "slot numbering error");
        obj = cf->slots_ptr[obj_slot];
        value_obj = cf->slots_ptr[value_slot];
        key = aski->key;
        assign_type = aski->type;
        has_char_key = true;
      }
      if (!has_char_key) { // can assume INSTR_ASSIGN
        // non-string key, goes to []=
        Object *index_assign_op = object_lookup(obj, "[]=", NULL);
        if (index_assign_op) {
          Object *function_base = object_lookup(root, "function", NULL);
          Object *closure_base = object_lookup(root, "closure", NULL);
          FunctionObject *fn_index_assign_op = (FunctionObject*) obj_instance_of(index_assign_op, function_base);
          ClosureObject *cl_index_assign_op = (ClosureObject*) obj_instance_of(index_assign_op, closure_base);
          VM_ASSERT(fn_index_assign_op || cl_index_assign_op, "'[]=' is neither function nor closure");
          Object *key_value_pair[] = {cf->slots_ptr[assign_instr->key_slot], value_obj};
          if (fn_index_assign_op) fn_index_assign_op->fn_ptr(state, obj, index_assign_op, key_value_pair, 2);
          else cl_index_assign_op->base.fn_ptr(state, obj, index_assign_op, key_value_pair, 2);
          break;
        }
        VM_ASSERT(false, "key is not string and no '[]=' is set");
      }
      // fprintf(stderr, "> obj set %p . '%s' = %p\n", (void*) obj, key, (void*) value_obj);
      VM_ASSERT(obj, "assignment to null object");
      switch (assign_type) {
        case ASSIGN_PLAIN:
          object_set(obj, key, value_obj);
          break;
        case ASSIGN_EXISTING:
        {
          char *error = object_set_existing(obj, key, value_obj);
          VM_ASSERT(!error, error);
          break;
        }
        case ASSIGN_SHADOWING:
          if (!object_set_shadowing(obj, key, value_obj)) {
            VM_ASSERT(false, "key '%s' not found in object", key);
          }
          break;
      }
    } break;
    case INSTR_ALLOC_OBJECT:{
      AllocObjectInstr *alloc_obj_instr = (AllocObjectInstr*) instr;
      int target_slot = alloc_obj_instr->target_slot, parent_slot = alloc_obj_instr->parent_slot;
      VM_ASSERT(target_slot < cf->slots_len, "slot numbering error");
      VM_ASSERT(parent_slot < cf->slots_len, "slot numbering error");
      Object *parent_obj = cf->slots_ptr[parent_slot];
      if (parent_obj) VM_ASSERT(!(parent_obj->flags & OBJ_NOINHERIT), "cannot inherit from object marked no-inherit");
      cf->slots_ptr[target_slot] = alloc_object(state, cf->slots_ptr[parent_slot]);
    } break;
    case INSTR_ALLOC_INT_OBJECT:{
      AllocIntObjectInstr *alloc_int_obj_instr = (AllocIntObjectInstr*) instr;
      int target_slot = alloc_int_obj_instr->target_slot, value = alloc_int_obj_instr->value;
      VM_ASSERT(target_slot < cf->slots_len, "slot numbering error");
      cf->slots_ptr[target_slot] = alloc_int(state, value);
    } break;
    case INSTR_ALLOC_FLOAT_OBJECT:{
      AllocFloatObjectInstr *alloc_float_obj_instr = (AllocFloatObjectInstr*) instr;
      int target_slot = alloc_float_obj_instr->target_slot; float value = alloc_float_obj_instr->value;
      VM_ASSERT(target_slot < cf->slots_len, "slot numbering error");
      cf->slots_ptr[target_slot] = alloc_float(state, value);
    } break;
    case INSTR_ALLOC_ARRAY_OBJECT:{
      AllocArrayObjectInstr *alloc_array_obj_instr = (AllocArrayObjectInstr*) instr;
      int target_slot = alloc_array_obj_instr->target_slot;
      VM_ASSERT(target_slot < cf->slots_len, "slot numbering error");
      Object *obj = alloc_array(state, NULL, 0);
      cf->slots_ptr[target_slot] = obj;
    } break;
    case INSTR_ALLOC_STRING_OBJECT:{
      AllocStringObjectInstr *alloc_string_obj_instr = (AllocStringObjectInstr*) instr;
      int target_slot = alloc_string_obj_instr->target_slot; char *value = alloc_string_obj_instr->value;
      VM_ASSERT(target_slot < cf->slots_len, "slot numbering error");
      cf->slots_ptr[target_slot] = alloc_string(state, value);
    } break;
    case INSTR_ALLOC_CLOSURE_OBJECT:{
      AllocClosureObjectInstr *alloc_closure_obj_instr = (AllocClosureObjectInstr*) instr;
      int target_slot = alloc_closure_obj_instr->target_slot, context_slot = alloc_closure_obj_instr->context_slot;
      VM_ASSERT(target_slot < cf->slots_len, "slot numbering error");
      VM_ASSERT(context_slot < cf->slots_len, "slot numbering error");
      cf->slots_ptr[target_slot] = alloc_closure_fn(cf->slots_ptr[context_slot], alloc_closure_obj_instr->fn);
    } break;
    case INSTR_CLOSE_OBJECT:{
      CloseObjectInstr *close_object_instr = (CloseObjectInstr*) instr;
      int slot = close_object_instr->slot;
      VM_ASSERT(slot < cf->slots_len, "slot numbering error");
      Object *obj = cf->slots_ptr[slot];
      VM_ASSERT(!(obj->flags & OBJ_CLOSED), "object is already closed!");
      obj->flags |= OBJ_CLOSED;
    } break;
    case INSTR_CALL: {
      CallInstr *call_instr = (CallInstr*) instr;
      int function_slot = call_instr->function_slot;
      int this_slot = call_instr->this_slot, args_length = call_instr->args_length;
      VM_ASSERT(function_slot < cf->slots_len, "slot numbering error");
      VM_ASSERT(this_slot < cf->slots_len, "slot numbering error");
      Object *this_obj = cf->slots_ptr[this_slot];
      Object *fn_obj = cf->slots_ptr[function_slot];
      // validate function type
      Object *closure_base = object_lookup(root, "closure", NULL);
      Object *function_base = object_lookup(root, "function", NULL);
      FunctionObject *fn = (FunctionObject*) obj_instance_of(fn_obj, function_base);
      ClosureObject *cl = (ClosureObject*) obj_instance_of(fn_obj, closure_base);
      VM_ASSERT(cl || fn, "cannot call: object is neither function nor closure");
      // form args array from slots
      
      Object **args;
      if (args_length < 10) { args = args_prealloc[args_length]; }
      else { args = malloc(sizeof(Object*) * args_length); }
      
      for (int i = 0; i < args_length; ++i) {
        int argslot = call_instr->args_ptr[i];
        VM_ASSERT(argslot < cf->slots_len, "slot numbering error");
        args[i] = cf->slots_ptr[argslot];
      }
      int prev_stacklen = state->stack_len;
      if (cl) cl->base.fn_ptr(state, this_obj, fn_obj, args, args_length);
      else fn->fn_ptr(state, this_obj, fn_obj, args, args_length);
      // recreate pointer, because the function may have reallocated our stack
      // and we still need to increment the instr ptr for this frame (which is now 1 down)
      cf = &state->stack_ptr[prev_stacklen - 1];
      
      if (args_length < 10) { }
      else { free(args); }
    } break;
    case INSTR_RETURN: {
      ReturnInstr *ret_instr = (ReturnInstr*) instr;
      int ret_slot = ret_instr->ret_slot;
      VM_ASSERT(ret_slot < cf->slots_len, "slot numbering error");
      Object *res = cf->slots_ptr[ret_slot];
      gc_remove_roots(state, &cf->frameroot);
      vm_remove_frame(state);
      state->result_value = res;
    } break;
    case INSTR_SAVE_RESULT: {
      SaveResultInstr *save_instr = (SaveResultInstr*) instr;
      int save_slot = save_instr->target_slot;
      VM_ASSERT(save_slot < cf->slots_len, "slot numbering error");
      cf->slots_ptr[save_slot] = state->result_value;
      state->result_value = NULL;
    } break;
    case INSTR_BR: {
      BranchInstr *br_instr = (BranchInstr*) instr;
      int blk = br_instr->blk;
      VM_ASSERT(blk < cf->uf->body.blocks_len, "slot numbering error");
      cf->block = &cf->uf->body.blocks_ptr[blk];
      cf->instr_offs = -1;
    } break;
    case INSTR_TESTBR: {
      TestBranchInstr *tbr_instr = (TestBranchInstr*) instr;
      int test_slot = tbr_instr->test_slot;
      int true_blk = tbr_instr->true_blk, false_blk = tbr_instr->false_blk;
      VM_ASSERT(test_slot < cf->slots_len, "slot numbering error");
      Object *test_value = cf->slots_ptr[test_slot];
      
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
      cf->block = &cf->uf->body.blocks_ptr[target_blk];
      cf->instr_offs = -1;
    } break;
    default:
      VM_ASSERT(false, "unknown instruction: %i\n", instr->type);
      break;
  }
  if (state->runstate == VM_ERRORED) return;
  cf->instr_offs++;
}

void vm_run(VMState *state) {
  assert(state->runstate == VM_TERMINATED || state->runstate == VM_ERRORED);
  // no call queued, no need to run
  // (this can happen when we executed a native function,
  //  expecting to set up a vm call)
  if (state->stack_len == 0) return;
  assert(state->stack_len > 0);
  state->runstate = VM_RUNNING;
  state->error = NULL;
  void **args_prealloc = malloc(sizeof(void*) * 10);
  for (int i = 0; i < 10; ++i) { args_prealloc[i] = malloc(sizeof(Object*) * i); }
  while (state->runstate == VM_RUNNING) {
    vm_step(state, args_prealloc);
    if (state->stack_len == 0) state->runstate = VM_TERMINATED;
  }
}

void call_function(VMState *state, Object *context, UserFunction *fn, Object **args_ptr, int args_len) {
  Callframe *cf = vm_alloc_frame(state, fn->slots);
  
  cf->uf = fn;
  cf->context = context;
  gc_add_roots(state, cf->slots_ptr, cf->slots_len, &cf->frameroot);
  
  if (args_len != cf->uf->arity) { vm_error(state, "arity violation in call!"); return; }
  for (int i = 0; i < args_len; ++i) {
    cf->slots_ptr[i] = args_ptr[i];
  }
  
  if (cf->uf->body.blocks_len == 0) {
    vm_error(state, "invalid function: no instructions");
    return;
  }
  cf->block = cf->uf->body.blocks_ptr;
  cf->instr_offs = 0;
}

void function_handler(VMState *state, Object *thisptr, Object *fn, Object **args_ptr, int args_len) {
  // discard calling context (lexical scoping!)
  ClosureObject *fn_obj = (ClosureObject*) fn;
  call_function(state, fn_obj->context, &fn_obj->vmfun, args_ptr, args_len);
}

void method_handler(VMState *state, Object *thisptr, Object *fn, Object **args_ptr, int args_len) {
  // discard calling context (lexical scoping!)
  ClosureObject *fn_obj = (ClosureObject*) fn;
  Object *context = alloc_object(state, fn_obj->context);
  object_set(context, "this", thisptr);
  call_function(state, context, &fn_obj->vmfun, args_ptr, args_len);
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
