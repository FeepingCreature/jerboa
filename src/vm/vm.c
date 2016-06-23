#include "vm/vm.h"

#include <stdarg.h>
#include <stdio.h>

#include "call.h"
#include "dump.h"
#include "gc.h"

#define UNLIKELY(X) __builtin_expect(X, 0)

const long long sample_stepsize = 200000LL; // 0.2ms

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
  cf->slots_ptr = cache_alloc(sizeof(Object*)*cf->slots_len);
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
  cache_free(sizeof(Object*)*cf->slots_len, cf->slots_ptr);
  // TODO shrink memory?
  state->stack_len = state->stack_len - 1;
}

void vm_print_backtrace(VMState *state) {
  VMState *curstate = state;
  int k = 0;
  while (curstate) {
    for (int i = curstate->stack_len - 1; i >= 0; --i, ++k) {
      Callframe *curf = &curstate->stack_ptr[i];
      Instr *instr = curf->instr_ptr;
      
      const char *file;
      TextRange line;
      int row, col;
      bool found = find_text_pos(instr->belongs_to->text_from, &file, &line, &row, &col);
      assert(found);
      fprintf(stderr, "#%i\t%s:%i\t%.*s\n", k, file, row+1, (int) (line.end - line.start - 1), line.start);
    }
    curstate = curstate->parent;
  }
}

static void vm_record_profile(VMState *state) {
  struct timespec prof_time;
  long long ns_diff = get_clock_and_difference(&prof_time, &state->shared->profstate.last_prof_time);
  if (ns_diff > sample_stepsize) {
    state->shared->profstate.last_prof_time = prof_time;
    
    HashTable *direct_tbl = &state->shared->profstate.direct_table;
    HashTable *indirect_tbl = &state->shared->profstate.indirect_table;
    
    VMState *curstate = state;
    // fprintf(stderr, "generate backtrace\n");
    int k = 0;
    while (curstate) {
      for (int i = curstate->stack_len - 1; i >= 0; --i, ++k) {
        Callframe *curf = &curstate->stack_ptr[i];
        Instr *instr = curf->instr_ptr;
        // ranges are unique (and instrs must live as long as the vm state lives anyways)
        // so we can just use the pointer stored in the instr as the key
        char *key_ptr = (char*) &instr->belongs_to;
        int key_len = sizeof(instr->belongs_to);
        
        size_t key_hash = hash(key_ptr, key_len);
        
        if (k == 0) {
          void **freeptr;
          void **entry_p = table_lookup_ref_alloc_with_hash(direct_tbl, key_hash, key_ptr, key_len, &freeptr);
          if (entry_p) (*(int*) entry_p) ++;
          else (*(int*) freeptr) = 1;
        } else {
          // don't double-count ranges in case of recursion
          bool range_already_counted = instr->belongs_to->last_cycle_seen == state->shared->cyclecount;
          
          if (!range_already_counted) {
            void **freeptr;
            void **entry_p = table_lookup_ref_alloc_with_hash(indirect_tbl, key_hash, key_ptr, key_len, &freeptr);
            if (entry_p) (*(int*) entry_p) ++;
            else (*(int*) freeptr) = 1;
          }
        }
        instr->belongs_to->last_cycle_seen = state->shared->cyclecount;
      }
      curstate = curstate->parent;
    }
    long long ns_taken = get_clock_and_difference(NULL, &prof_time);
    if (ns_taken > sample_stepsize / 10) {
      fprintf(stderr, "warning: collecting profiling info took %lli%% of the last step.\n", ns_taken * 100LL / sample_stepsize);
    }
  }
}

static void vm_step(VMState *state) {
  Object *root = state->root;
  Callframe *cf = &state->stack_ptr[state->stack_len - 1];
  
  state->shared->cyclecount ++;
  Instr *instr = cf->instr_ptr;
  Instr *next_instr = NULL;
  switch (instr->type) {
    case INSTR_GET_ROOT:{
      GetRootInstr *get_root_instr = (GetRootInstr*) instr;
      int slot = get_root_instr->slot;
      VM_ASSERT(slot < cf->slots_len, "internal slot error");
      cf->slots_ptr[get_root_instr->slot] = root;
      next_instr = (Instr*)(get_root_instr + 1);
    } break;
    case INSTR_GET_CONTEXT:{
      GetContextInstr *get_context_instr = (GetContextInstr*) instr;
      int slot = get_context_instr->slot;
      VM_ASSERT(slot < cf->slots_len, "internal slot error");
      cf->slots_ptr[slot] = cf->context;
      next_instr = (Instr*)(get_context_instr + 1);
    } break;
    case INSTR_ACCESS: case INSTR_ACCESS_STRING_KEY: {
      AccessInstr *access_instr = (AccessInstr*) instr;
      AccessStringKeyInstr *aski = (AccessStringKeyInstr*) instr;
      int obj_slot, target_slot;
      if (instr->type == INSTR_ACCESS) {
        obj_slot = access_instr->obj_slot;
        target_slot = access_instr->target_slot;
        next_instr = (Instr*)(access_instr + 1);
      } else {
        obj_slot = aski->obj_slot;
        target_slot = aski->target_slot;
        next_instr = (Instr*)(aski + 1);
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
          gc_add_perm(state, key_obj);
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
            key_obj = alloc_string(state, aski->key, strlen(aski->key));
          }
          
          VMState substate = {0};
          substate.parent = state;
          substate.root = state->root;
          substate.shared = state->shared;
          
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
          // Not sure how to correctly handle "key leakage".
          // TODO figure out better.
          gc_add_perm(state, key_obj);
          assign_type = assign_instr->type;
          has_char_key = true;
        }
        next_instr = (Instr*)(assign_instr + 1);
      } else {
        int obj_slot = aski->obj_slot, value_slot = aski->value_slot;
        VM_ASSERT(obj_slot < cf->slots_len, "slot numbering error");
        VM_ASSERT(value_slot < cf->slots_len, "slot numbering error");
        obj = cf->slots_ptr[obj_slot];
        value_obj = cf->slots_ptr[value_slot];
        key = aski->key;
        assign_type = aski->type;
        has_char_key = true;
        next_instr = (Instr*)(aski + 1);
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
      next_instr = (Instr*)(alloc_obj_instr + 1);
    } break;
    case INSTR_ALLOC_INT_OBJECT:{
      AllocIntObjectInstr *alloc_int_obj_instr = (AllocIntObjectInstr*) instr;
      int target_slot = alloc_int_obj_instr->target_slot, value = alloc_int_obj_instr->value;
      VM_ASSERT(target_slot < cf->slots_len, "slot numbering error");
      if (UNLIKELY(!alloc_int_obj_instr->int_obj)) {
        Object *obj = alloc_int(state, value);
        alloc_int_obj_instr->int_obj = obj;
        gc_add_perm(state, obj);
      }
      cf->slots_ptr[target_slot] = alloc_int_obj_instr->int_obj;
      next_instr = (Instr*)(alloc_int_obj_instr + 1);
    } break;
    case INSTR_ALLOC_FLOAT_OBJECT:{
      AllocFloatObjectInstr *alloc_float_obj_instr = (AllocFloatObjectInstr*) instr;
      int target_slot = alloc_float_obj_instr->target_slot; float value = alloc_float_obj_instr->value;
      VM_ASSERT(target_slot < cf->slots_len, "slot numbering error");
      // okay so
      // enabling this is slower
      // just HAVING IT IN THE CODE makes it slower
      // even for scripts that never use floats
      // what the fuck
      /*
      if (UNLIKELY(!alloc_float_obj_instr->float_obj)) {
        Object *obj = alloc_float(state, value);
        alloc_float_obj_instr->float_obj = obj;
        gc_add_perm(state, obj);
      }
      cf->slots_ptr[target_slot] = alloc_float_obj_instr->float_obj;
      */
      cf->slots_ptr[target_slot] = alloc_float(state, value);
      next_instr = (Instr*)(alloc_float_obj_instr + 1);
    } break;
    case INSTR_ALLOC_ARRAY_OBJECT:{
      AllocArrayObjectInstr *alloc_array_obj_instr = (AllocArrayObjectInstr*) instr;
      int target_slot = alloc_array_obj_instr->target_slot;
      VM_ASSERT(target_slot < cf->slots_len, "slot numbering error");
      Object *obj = alloc_array(state, NULL, (IntObject*) state->shared->vcache.int_zero);
      cf->slots_ptr[target_slot] = obj;
      next_instr = (Instr*)(alloc_array_obj_instr + 1);
    } break;
    case INSTR_ALLOC_STRING_OBJECT:{
      AllocStringObjectInstr *alloc_string_obj_instr = (AllocStringObjectInstr*) instr;
      int target_slot = alloc_string_obj_instr->target_slot; char *value = alloc_string_obj_instr->value;
      VM_ASSERT(target_slot < cf->slots_len, "slot numbering error");
      if (UNLIKELY(!alloc_string_obj_instr->str_obj)) {
        Object *obj = alloc_string(state, value, strlen(value));
        alloc_string_obj_instr->str_obj = obj;
        gc_add_perm(state, obj);
      }
      cf->slots_ptr[target_slot] = alloc_string_obj_instr->str_obj;
      next_instr = (Instr*)(alloc_string_obj_instr + 1);
    } break;
    case INSTR_ALLOC_CLOSURE_OBJECT:{
      AllocClosureObjectInstr *alloc_closure_obj_instr = (AllocClosureObjectInstr*) instr;
      int target_slot = alloc_closure_obj_instr->target_slot, context_slot = alloc_closure_obj_instr->context_slot;
      VM_ASSERT(target_slot < cf->slots_len, "slot numbering error");
      VM_ASSERT(context_slot < cf->slots_len, "slot numbering error");
      cf->slots_ptr[target_slot] = alloc_closure_fn(cf->slots_ptr[context_slot], alloc_closure_obj_instr->fn);
      next_instr = (Instr*)(alloc_closure_obj_instr + 1);
    } break;
    case INSTR_CLOSE_OBJECT:{
      CloseObjectInstr *close_object_instr = (CloseObjectInstr*) instr;
      int slot = close_object_instr->slot;
      VM_ASSERT(slot < cf->slots_len, "slot numbering error");
      Object *obj = cf->slots_ptr[slot];
      VM_ASSERT(!(obj->flags & OBJ_CLOSED), "object is already closed!");
      obj->flags |= OBJ_CLOSED;
      next_instr = (Instr*)(close_object_instr + 1);
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
      if (args_length < 10) { args = state->shared->vcache.args_prealloc[args_length]; }
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
      
      next_instr = (Instr*)(call_instr + 1);
      
      // continue straight to INSTR_SAVE_RESULT (only if fn_obj was an intrinsic)
      if (state->stack_len == prev_stacklen && next_instr->type == INSTR_SAVE_RESULT) {
        instr = next_instr;
      } else break;
    }
    case INSTR_SAVE_RESULT: {
      SaveResultInstr *save_instr = (SaveResultInstr*) instr;
      int save_slot = save_instr->target_slot;
      VM_ASSERT(save_slot < cf->slots_len, "slot numbering error");
      cf->slots_ptr[save_slot] = state->result_value;
      state->result_value = NULL;
      next_instr = (Instr*)(save_instr + 1);
    } break;
    case INSTR_RETURN: {
      ReturnInstr *ret_instr = (ReturnInstr*) instr;
      int ret_slot = ret_instr->ret_slot;
      VM_ASSERT(ret_slot < cf->slots_len, "slot numbering error");
      Object *res = cf->slots_ptr[ret_slot];
      gc_remove_roots(state, &cf->frameroot);
      vm_remove_frame(state);
      state->result_value = res;
      next_instr = (Instr*)(ret_instr + 1);
    } break;
    case INSTR_BR: {
      BranchInstr *br_instr = (BranchInstr*) instr;
      int blk = br_instr->blk;
      VM_ASSERT(blk < cf->uf->body.blocks_len, "slot numbering error");
      next_instr = cf->uf->body.blocks_ptr[blk].instrs_ptr;
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
      next_instr = cf->uf->body.blocks_ptr[target_blk].instrs_ptr;
    } break;
    default:
      VM_ASSERT(false, "unknown instruction: %i\n", instr->type);
      break;
  }
  if (state->runstate == VM_ERRORED) return;
  if (UNLIKELY(state->shared->cyclecount > state->shared->profstate.next_prof_check)) {
    state->shared->profstate.next_prof_check = state->shared->cyclecount + 1021;
    vm_record_profile(state);
  }
  // printf("change %p (%i) to %p; %li when instr had %i\n", (void*) cf->instr_ptr, cf->instr_ptr->type, (void*) next_instr, (char*) next_instr - (char*) cf->instr_ptr, instr_size(cf->instr_ptr));
  cf->instr_ptr = next_instr;
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
  // TODO move to state init
  if (!state->shared->vcache.args_prealloc) {
    state->shared->vcache.args_prealloc = malloc(sizeof(Object**) * 10);
    for (int i = 0; i < 10; ++i) { state->shared->vcache.args_prealloc[i] = malloc(sizeof(Object*) * i); }
  }
  // this should, frankly, really be done in vm_step
  // but meh, it's faster to only do it once
  GCRootSet result_set;
  gc_add_roots(state, &state->result_value, 1, &result_set);
  while (state->runstate == VM_RUNNING) {
    vm_step(state);
    if (state->stack_len == 0) state->runstate = VM_TERMINATED;
  }
  gc_remove_roots(state, &result_set);
}
