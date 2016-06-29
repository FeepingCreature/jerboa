#include "vm/vm.h"

#include <stdarg.h>
#include <stdio.h>

#include "vm/call.h"
#include "vm/dump.h"
#include "gc.h"

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
        gc_remove_roots(state, &state->stack_ptr[i].frameroot_slots);
        gc_remove_roots(state, &state->stack_ptr[i].frameroot_ctx);
      }
      memcpy(ptr, old_ptr, capacity);
      state->stack_ptr = ptr;
      // add new gc roots
      for (int i = 0; i < state->stack_len; ++i) {
        Callframe *cf = &state->stack_ptr[i];
        gc_add_roots(state, cf->slots_ptr, cf->slots_len, &cf->frameroot_slots);
        gc_add_roots(state, &cf->context, 1, &cf->frameroot_ctx);
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
  state->backtrace_depth = 0;
}

void vm_remove_frame(VMState *state) {
  Callframe *cf = &state->stack_ptr[state->stack_len - 1];
  cache_free(sizeof(Object*)*cf->slots_len, cf->slots_ptr);
  // TODO shrink memory?
  state->stack_len = state->stack_len - 1;
}

void vm_print_backtrace(VMState *state) {
  int k = state->backtrace_depth;
  if (state->backtrace) fprintf(stderr, "%s", state->backtrace);
  for (int i = state->stack_len - 1; i >= 0; --i, ++k) {
    Callframe *curf = &state->stack_ptr[i];
    Instr *instr = curf->instr_ptr;
    
    const char *file;
    TextRange line;
    int row, col;
    bool found = find_text_pos(instr->belongs_to->text_from, &file, &line, &row, &col);
    (void) found; assert(found);
    fprintf(stderr, "#%i\t%s:%i\t%.*s\n", k, file, row+1, (int) (line.end - line.start - 1), line.start);
  }
}

char *vm_record_backtrace(VMState *state, int *depth) {
  char *res_ptr = NULL; int res_len = 0;
  int k = state->backtrace_depth;
  if (state->backtrace) {
    res_len = strlen(state->backtrace);
    res_ptr = malloc(res_len + 1);
    strncpy(res_ptr, state->backtrace, res_len + 1);
  }
  for (int i = state->stack_len - 1; i >= 0; --i, ++k) {
    Callframe *curf = &state->stack_ptr[i];
    Instr *instr = curf->instr_ptr;
    
    const char *file;
    TextRange line;
    int row, col;
    bool found = find_text_pos(instr->belongs_to->text_from, &file, &line, &row, &col);
    (void) found; assert(found);
    int size = snprintf(NULL, 0, "#%i\t%s:%i\t%.*s\n", k, file, row+1, (int) (line.end - line.start - 1), line.start);
    res_ptr = realloc(res_ptr, res_len + size + 1);
    snprintf(res_ptr + res_len, size + 1, "#%i\t%s:%i\t%.*s\n", k, file, row+1, (int) (line.end - line.start - 1), line.start);
    res_len += size;
  }
  res_ptr[res_len] = 0;
  *depth = k;
  return res_ptr;
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
          void **entry_p = table_lookup_ref_alloc_with_hash(direct_tbl, key_ptr, key_len, key_hash, &freeptr);
          if (entry_p) (*(int*) entry_p) ++;
          else (*(int*) freeptr) = 1;
        } else {
          // don't double-count ranges in case of recursion
          bool range_already_counted = instr->belongs_to->last_cycle_seen == state->shared->cyclecount;
          
          if (!range_already_counted) {
            void **freeptr;
            void **entry_p = table_lookup_ref_alloc_with_hash(indirect_tbl, key_ptr, key_len, key_hash, &freeptr);
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

typedef struct {
  Object *root;
  Callframe *cf;
  Instr *instr;
  VMState *reststate;
} FastVMState;

struct _FnWrap;
typedef struct _FnWrap FnWrap;

typedef FnWrap (*VMInstrFn)(FastVMState *state);
struct _FnWrap {
  VMInstrFn self;
};

static FnWrap vm_halt(FastVMState *state);

#define VM_ASSERT2(cond, ...) if (UNLIKELY(!(cond)) && (vm_error(state->reststate, __VA_ARGS__), true)) return (FnWrap) { vm_halt }

#ifndef NDEBUG
#define VM_ASSERT2_SLOT(cond, ...) VM_ASSERT2(cond, __VA_ARGS__)
#else
#define VM_ASSERT2_SLOT(cond, ...) (void) 0
#endif

static VMInstrFn instr_fns[INSTR_LAST] = {0};

static FnWrap vm_instr_get_root(FastVMState *state) {
  GetRootInstr *get_root_instr = (GetRootInstr*) state->instr;
  int slot = get_root_instr->slot;
  VM_ASSERT2_SLOT(slot < state->cf->slots_len, "internal slot error");
  state->cf->slots_ptr[slot] = state->root;
  state->instr = (Instr*)(get_root_instr + 1);
  return (FnWrap) { instr_fns[state->instr->type] };
}

static FnWrap vm_instr_get_context(FastVMState *state) {
  GetContextInstr *get_context_instr = (GetContextInstr*) state->instr;
  int slot = get_context_instr->slot;
  VM_ASSERT2_SLOT(slot < state->cf->slots_len, "internal slot error");
  state->cf->slots_ptr[slot] = state->cf->context;
  state->instr = (Instr*)(get_context_instr + 1);
  return (FnWrap) { instr_fns[state->instr->type] };
}

static FnWrap vm_instr_alloc_object(FastVMState *state) {
  AllocObjectInstr *alloc_obj_instr = (AllocObjectInstr*) state->instr;
  int target_slot = alloc_obj_instr->target_slot, parent_slot = alloc_obj_instr->parent_slot;
  VM_ASSERT2_SLOT(target_slot < state->cf->slots_len, "slot numbering error");
  VM_ASSERT2_SLOT(parent_slot < state->cf->slots_len, "slot numbering error");
  Object *parent_obj = state->cf->slots_ptr[parent_slot];
  VM_ASSERT2(!parent_obj || !(parent_obj->flags & OBJ_NOINHERIT), "cannot inherit from object marked no-inherit");
  state->cf->slots_ptr[target_slot] = alloc_object(state->reststate, state->cf->slots_ptr[parent_slot]);
  state->instr = (Instr*)(alloc_obj_instr + 1);
  return (FnWrap) { instr_fns[state->instr->type] };
}

static FnWrap vm_instr_alloc_int_object(FastVMState *state) {
  AllocIntObjectInstr *alloc_int_obj_instr = (AllocIntObjectInstr*) state->instr;
  int target_slot = alloc_int_obj_instr->target_slot, value = alloc_int_obj_instr->value;
  VM_ASSERT2_SLOT(target_slot < state->cf->slots_len, "slot numbering error");
  if (UNLIKELY(!alloc_int_obj_instr->int_obj)) {
    Object *obj = alloc_int(state->reststate, value);
    alloc_int_obj_instr->int_obj = obj;
    gc_add_perm(state->reststate, obj);
  }
  state->cf->slots_ptr[target_slot] = alloc_int_obj_instr->int_obj;
  state->instr = (Instr*)(alloc_int_obj_instr + 1);
  return (FnWrap) { instr_fns[state->instr->type] };
}

static FnWrap vm_instr_alloc_float_object(FastVMState *state) {
  AllocFloatObjectInstr *alloc_float_obj_instr = (AllocFloatObjectInstr*) state->instr;
  int target_slot = alloc_float_obj_instr->target_slot; float value = alloc_float_obj_instr->value;
  VM_ASSERT2_SLOT(target_slot < state->cf->slots_len, "slot numbering error");
  if (UNLIKELY(!alloc_float_obj_instr->float_obj)) {
    Object *obj = alloc_float(state->reststate, value);
    alloc_float_obj_instr->float_obj = obj;
    gc_add_perm(state->reststate, obj);
  }
  state->cf->slots_ptr[target_slot] = alloc_float_obj_instr->float_obj;
  state->instr = (Instr*)(alloc_float_obj_instr + 1);
  return (FnWrap) { instr_fns[state->instr->type] };
}

static FnWrap vm_instr_alloc_array_object(FastVMState *state) {
  AllocArrayObjectInstr *alloc_array_obj_instr = (AllocArrayObjectInstr*) state->instr;
  int target_slot = alloc_array_obj_instr->target_slot;
  VM_ASSERT2_SLOT(target_slot < state->cf->slots_len, "slot numbering error");
  Object *obj = alloc_array(state->reststate, NULL, (IntObject*) state->reststate->shared->vcache.int_zero);
  state->cf->slots_ptr[target_slot] = obj;
  state->instr = (Instr*)(alloc_array_obj_instr + 1);
  return (FnWrap) { instr_fns[state->instr->type] };
}

static FnWrap vm_instr_alloc_string_object(FastVMState *state) {
  AllocStringObjectInstr *alloc_string_obj_instr = (AllocStringObjectInstr*) state->instr;
  int target_slot = alloc_string_obj_instr->target_slot; char *value = alloc_string_obj_instr->value;
  VM_ASSERT2_SLOT(target_slot < state->cf->slots_len, "slot numbering error");
  if (UNLIKELY(!alloc_string_obj_instr->str_obj)) {
    Object *obj = alloc_string(state->reststate, value, strlen(value));
    alloc_string_obj_instr->str_obj = obj;
    gc_add_perm(state->reststate, obj);
  }
  state->cf->slots_ptr[target_slot] = alloc_string_obj_instr->str_obj;
  state->instr = (Instr*)(alloc_string_obj_instr + 1);
  return (FnWrap) { instr_fns[state->instr->type] };
}

static FnWrap vm_instr_alloc_closure_object(FastVMState *state) {
  AllocClosureObjectInstr *alloc_closure_obj_instr = (AllocClosureObjectInstr*) state->instr;
  int target_slot = alloc_closure_obj_instr->target_slot, context_slot = alloc_closure_obj_instr->context_slot;
  VM_ASSERT2_SLOT(target_slot < state->cf->slots_len, "slot numbering error");
  VM_ASSERT2_SLOT(context_slot < state->cf->slots_len, "slot numbering error");
  state->cf->slots_ptr[target_slot] = alloc_closure_fn(state->cf->slots_ptr[context_slot], alloc_closure_obj_instr->fn);
  state->instr = (Instr*)(alloc_closure_obj_instr + 1);
  return (FnWrap) { instr_fns[state->instr->type] };
}

static FnWrap vm_instr_close_object(FastVMState *state) {
  CloseObjectInstr *close_object_instr = (CloseObjectInstr*) state->instr;
  int slot = close_object_instr->slot;
  VM_ASSERT2_SLOT(slot < state->cf->slots_len, "slot numbering error");
  Object *obj = state->cf->slots_ptr[slot];
  VM_ASSERT2(!(obj->flags & OBJ_CLOSED), "object is already closed!");
  obj->flags |= OBJ_CLOSED;
  state->instr = (Instr*)(close_object_instr + 1);
  return (FnWrap) { instr_fns[state->instr->type] };
}

static FnWrap vm_instr_freeze_object(FastVMState *state) {
  FreezeObjectInstr *freeze_object_instr = (FreezeObjectInstr*) state->instr;
  int slot = freeze_object_instr->slot;
  VM_ASSERT2_SLOT(slot < state->cf->slots_len, "slot numbering error");
  Object *obj = state->cf->slots_ptr[slot];
  VM_ASSERT2(!(obj->flags & OBJ_FROZEN), "object is already frozen!");
  obj->flags |= OBJ_FROZEN;
  state->instr = (Instr*)(freeze_object_instr + 1);
  return (FnWrap) { instr_fns[state->instr->type] };
}

static FnWrap vm_instr_access(FastVMState *state) {
  AccessInstr *access_instr = (AccessInstr*) state->instr;
  int obj_slot, target_slot;
  obj_slot = access_instr->obj_slot;
  target_slot = access_instr->target_slot;
  
  VM_ASSERT2_SLOT(obj_slot < state->cf->slots_len, "internal slot error");
  VM_ASSERT2_SLOT(target_slot < state->cf->slots_len, "internal slot error");
  Object *obj = state->cf->slots_ptr[obj_slot];
  
  char *key;
  bool has_char_key = false;
      
  int key_slot = access_instr->key_slot;
  VM_ASSERT2_SLOT(key_slot < state->cf->slots_len, "internal slot error");
  VM_ASSERT2(state->cf->slots_ptr[key_slot], "key slot null"); // TODO "slot_assigned"
  Object *string_base = object_lookup(state->root, "string", NULL);
  Object *key_obj = state->cf->slots_ptr[key_slot];
  VM_ASSERT2(key_obj, "key is null");
  StringObject *skey = (StringObject*) obj_instance_of(key_obj, string_base);
  bool object_found = false;
  if (skey) {
    gc_add_perm(state->reststate, key_obj);
    key = skey->value;
    state->cf->slots_ptr[target_slot] = object_lookup(obj, key, &object_found);
  }
  if (!object_found) {
    Object *index_op = object_lookup(obj, "[]", NULL);
    if (index_op) {
      Object *function_base = object_lookup(state->root, "function", NULL);
      Object *closure_base = object_lookup(state->root, "closure", NULL);
      FunctionObject *fn_index_op = (FunctionObject*) obj_instance_of(index_op, function_base);
      ClosureObject *cl_index_op = (ClosureObject*) obj_instance_of(index_op, closure_base);
      VM_ASSERT2(fn_index_op || cl_index_op, "index op is neither function nor closure");
      Object *key_obj = state->cf->slots_ptr[access_instr->key_slot];
      
      VMState substate = {0};
      // TODO update_state()
      substate.parent = state->reststate;
      substate.root = state->root;
      substate.shared = state->reststate->shared;
      
      if (fn_index_op) fn_index_op->fn_ptr(&substate, obj, index_op, &key_obj, 1);
      else cl_index_op->base.fn_ptr(&substate, obj, index_op, &key_obj, 1);
      
      vm_run(&substate);
      VM_ASSERT2(substate.runstate != VM_ERRORED, "[] overload failed: %s\n", substate.error);
      
      state->cf->slots_ptr[target_slot] = substate.result_value;
      
      object_found = true; // rely on the [] call to error on its own, if key not found
    }
  }
  if (!object_found) {
    if (has_char_key) {
      VM_ASSERT2(false, "property not found: '%s'", key);
    } else {
      VM_ASSERT2(false, "property not found!");
    }
  }
  state->instr = (Instr*)(access_instr + 1);
  return (FnWrap) { instr_fns[state->instr->type] };
}

static FnWrap vm_instr_access_string_key(FastVMState *state) {
  AccessStringKeyInstr *aski = (AccessStringKeyInstr*) state->instr;
  int obj_slot = aski->obj_slot;
  int target_slot = aski->target_slot;
  
  VM_ASSERT2_SLOT(obj_slot < state->cf->slots_len, "internal slot error");
  VM_ASSERT2_SLOT(target_slot < state->cf->slots_len, "internal slot error");
  Object *obj = state->cf->slots_ptr[obj_slot];
  
  char *key = aski->key;
  bool object_found = false;
  state->cf->slots_ptr[target_slot] = object_lookup(obj, key, &object_found);
  
  if (!object_found) {
    Object *index_op = object_lookup(obj, "[]", NULL);
    if (index_op) {
      Object *function_base = object_lookup(state->root, "function", NULL);
      Object *closure_base = object_lookup(state->root, "closure", NULL);
      FunctionObject *fn_index_op = (FunctionObject*) obj_instance_of(index_op, function_base);
      ClosureObject *cl_index_op = (ClosureObject*) obj_instance_of(index_op, closure_base);
      VM_ASSERT2(fn_index_op || cl_index_op, "index op is neither function nor closure");
      Object *key_obj = alloc_string(state->reststate, aski->key, strlen(aski->key));
      
      VMState substate = {0};
      // TODO see above
      substate.parent = state->reststate;
      substate.root = state->root;
      substate.shared = state->reststate->shared;
      
      if (fn_index_op) fn_index_op->fn_ptr(&substate, obj, index_op, &key_obj, 1);
      else cl_index_op->base.fn_ptr(&substate, obj, index_op, &key_obj, 1);
      
      vm_run(&substate);
      VM_ASSERT2(substate.runstate != VM_ERRORED, "[] overload failed: %s\n", substate.error);
      
      state->cf->slots_ptr[target_slot] = substate.result_value;
      
      object_found = true; // rely on the [] call to error on its own, if key not found
    }
  }
  if (!object_found) {
    VM_ASSERT2(false, "property not found: '%s'", key);
  }
  state->instr = (Instr*)(aski + 1);
  return (FnWrap) { instr_fns[state->instr->type] };
}

static FnWrap vm_instr_assign(FastVMState *state) {
  AssignInstr *assign_instr = (AssignInstr*) state->instr;
  int obj_slot = assign_instr->obj_slot, value_slot = assign_instr->value_slot;
  int key_slot = assign_instr->key_slot;
  VM_ASSERT2_SLOT(obj_slot < state->cf->slots_len, "slot numbering error");
  VM_ASSERT2_SLOT(value_slot < state->cf->slots_len, "slot numbering error");
  VM_ASSERT2_SLOT(key_slot < state->cf->slots_len, "slot numbering error");
  VM_ASSERT2(state->cf->slots_ptr[key_slot], "key slot null"); // TODO see above
  Object *obj = state->cf->slots_ptr[obj_slot];
  Object *value_obj = state->cf->slots_ptr[value_slot];
  Object *string_base = object_lookup(state->root, "string", NULL);
  Object *key_obj = state->cf->slots_ptr[key_slot];
  StringObject *skey = (StringObject*) obj_instance_of(key_obj, string_base);
  if (!skey) {
    // non-string key, goes to []=
    Object *index_assign_op = object_lookup(obj, "[]=", NULL);
    if (index_assign_op) {
      Object *function_base = object_lookup(state->root, "function", NULL);
      Object *closure_base = object_lookup(state->root, "closure", NULL);
      FunctionObject *fn_index_assign_op = (FunctionObject*) obj_instance_of(index_assign_op, function_base);
      ClosureObject *cl_index_assign_op = (ClosureObject*) obj_instance_of(index_assign_op, closure_base);
      VM_ASSERT2(fn_index_assign_op || cl_index_assign_op, "'[]=' is neither function nor closure");
      Object *key_value_pair[] = {state->cf->slots_ptr[assign_instr->key_slot], value_obj};
      if (fn_index_assign_op) fn_index_assign_op->fn_ptr(state->reststate, obj, index_assign_op, key_value_pair, 2);
      else cl_index_assign_op->base.fn_ptr(state->reststate, obj, index_assign_op, key_value_pair, 2);
      // TODO run vm here - bad bug right now!
      abort();
      state->instr = (Instr*)(assign_instr + 1);
      return (FnWrap) { instr_fns[state->instr->type] };
    }
    VM_ASSERT2(false, "key is not string and no '[]=' is set");
  }
  char *key = skey->value;
  // Not sure how to correctly handle "key leakage".
  // TODO figure out better.
  // TODO update reststate?
  gc_add_perm(state->reststate, key_obj);
  AssignType assign_type = assign_instr->type;
  // fprintf(stderr, "> obj set %p . '%s' = %p\n", (void*) obj, key, (void*) value_obj);
  VM_ASSERT2(obj, "assignment to null object");
  switch (assign_type) {
    case ASSIGN_PLAIN:
      object_set(obj, key, value_obj);
      break;
    case ASSIGN_EXISTING:
    {
      char *error = object_set_existing(obj, key, value_obj);
      VM_ASSERT2(!error, error);
      break;
    }
    case ASSIGN_SHADOWING:
      if (!object_set_shadowing(obj, key, value_obj)) {
        VM_ASSERT2(false, "key '%s' not found in object", key);
      }
      break;
  }
  state->instr = (Instr*)(assign_instr + 1);
  return (FnWrap) { instr_fns[state->instr->type] };
}

static FnWrap vm_instr_assign_string_key(FastVMState *state) {
  AssignStringKeyInstr *aski = (AssignStringKeyInstr*) state->instr;
  int obj_slot = aski->obj_slot, value_slot = aski->value_slot;
  VM_ASSERT2_SLOT(obj_slot < state->cf->slots_len, "slot numbering error");
  VM_ASSERT2_SLOT(value_slot < state->cf->slots_len, "slot numbering error");
  Object *obj = state->cf->slots_ptr[obj_slot];
  Object *value_obj = state->cf->slots_ptr[value_slot];
  char *key = aski->key;
  AssignType assign_type = aski->type;
  VM_ASSERT2(obj, "assignment to null object");
  switch (assign_type) {
    case ASSIGN_PLAIN:
      object_set(obj, key, value_obj);
      break;
    case ASSIGN_EXISTING:
    {
      char *error = object_set_existing(obj, key, value_obj);
      VM_ASSERT2(!error, error);
      break;
    }
    case ASSIGN_SHADOWING:
      if (!object_set_shadowing(obj, key, value_obj)) {
        VM_ASSERT2(false, "key '%s' not found in object", key);
      }
      break;
  }
  state->instr = (Instr*)(aski + 1);
  return (FnWrap) { instr_fns[state->instr->type] };
}

static FnWrap vm_instr_call(FastVMState *state) {
  CallInstr *call_instr = (CallInstr*) state->instr;
  int function_slot = call_instr->function_slot;
  int this_slot = call_instr->this_slot, args_length = call_instr->args_length;
  VM_ASSERT2_SLOT(function_slot < state->cf->slots_len, "slot numbering error");
  VM_ASSERT2_SLOT(this_slot < state->cf->slots_len, "slot numbering error");
  Object *this_obj = state->cf->slots_ptr[this_slot];
  Object *fn_obj = state->cf->slots_ptr[function_slot];
  // validate function type
  Object *closure_base = object_lookup(state->root, "closure", NULL);
  Object *function_base = object_lookup(state->root, "function", NULL);
  FunctionObject *fn = (FunctionObject*) obj_instance_of(fn_obj, function_base);
  ClosureObject *cl = (ClosureObject*) obj_instance_of(fn_obj, closure_base);
  VM_ASSERT2(cl || fn, "cannot call: object is neither function nor closure");
  // form args array from slots
  
  Object **args;
  if (args_length < 10) { args = state->reststate->shared->vcache.args_prealloc[args_length]; }
  else { args = malloc(sizeof(Object*) * args_length); }
  
  for (int i = 0; i < args_length; ++i) {
    int argslot = call_instr->args_ptr[i];
    VM_ASSERT2_SLOT(argslot < state->cf->slots_len, "slot numbering error");
    args[i] = state->cf->slots_ptr[argslot];
  }
  
  int prev_stacklen = state->reststate->stack_len;
  
  if (cl) cl->base.fn_ptr(state->reststate, this_obj, fn_obj, args, args_length);
  else fn->fn_ptr(state->reststate, this_obj, fn_obj, args, args_length);
  
  // intrinsic may have errored.
  if (state->reststate->runstate == VM_ERRORED) return (FnWrap) { vm_halt };
  
  // recalculate pointer, because the function may have reallocated our stack
  Callframe *old_cf = &state->reststate->stack_ptr[prev_stacklen - 1];
  old_cf->instr_ptr = (Instr*)(call_instr + 1); // step past call in the old stackframe
  
  if (args_length < 10) { }
  else { free(args); }
  
  // note: if fn_ptr was an intrinsic, old_cf = state->cf so this is okay.
  state->cf = &state->reststate->stack_ptr[state->reststate->stack_len - 1];
  state->instr = state->cf->instr_ptr; // assign first instr of call.
  return (FnWrap) { instr_fns[state->instr->type] };
}

static FnWrap vm_instr_save_result(FastVMState *state) {
  SaveResultInstr *save_instr = (SaveResultInstr*) state->instr;
  int save_slot = save_instr->target_slot;
  VM_ASSERT2_SLOT(save_slot < state->cf->slots_len, "slot numbering error");
  state->cf->slots_ptr[save_slot] = state->reststate->result_value;
  state->reststate->result_value = NULL;
  
  state->instr = (Instr*)(save_instr + 1);
  return (FnWrap) { instr_fns[state->instr->type] };
}

static FnWrap vm_halt(FastVMState *state) {
  (void) state;
  return (FnWrap) { vm_halt };
}

static FnWrap vm_instr_return(FastVMState *state) {
  ReturnInstr *ret_instr = (ReturnInstr*) state->instr;
  int ret_slot = ret_instr->ret_slot;
  VM_ASSERT2_SLOT(ret_slot < state->cf->slots_len, "slot numbering error");
  Object *res = state->cf->slots_ptr[ret_slot];
  gc_remove_roots(state->reststate, &state->cf->frameroot_slots);
  gc_remove_roots(state->reststate, &state->cf->frameroot_ctx);
  vm_remove_frame(state->reststate);
  state->reststate->result_value = res;
  
  if (state->reststate->stack_len == 0) return (FnWrap) { vm_halt };
  state->cf = &state->reststate->stack_ptr[state->reststate->stack_len - 1];
  state->instr = state->cf->instr_ptr;
  return (FnWrap) { instr_fns[state->instr->type] };
}

static FnWrap vm_instr_br(FastVMState *state) {
  BranchInstr *br_instr = (BranchInstr*) state->instr;
  int blk = br_instr->blk;
  VM_ASSERT2_SLOT(blk < state->cf->uf->body.blocks_len, "slot numbering error");
  state->instr = state->cf->uf->body.blocks_ptr[blk].instrs_ptr;
  return (FnWrap) { instr_fns[state->instr->type] };
}

static FnWrap vm_instr_testbr(FastVMState *state) {
  TestBranchInstr *tbr_instr = (TestBranchInstr*) state->instr;
  int test_slot = tbr_instr->test_slot;
  int true_blk = tbr_instr->true_blk, false_blk = tbr_instr->false_blk;
  VM_ASSERT2_SLOT(test_slot < state->cf->slots_len, "slot numbering error");
  Object *test_value = state->cf->slots_ptr[test_slot];
  
  Object *bool_base = object_lookup(state->root, "bool", NULL);
  Object *int_base = object_lookup(state->root, "int", NULL);
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
  state->instr = state->cf->uf->body.blocks_ptr[target_blk].instrs_ptr;
  return (FnWrap) { instr_fns[state->instr->type] };
}

static void vm_step(VMState *state) {
  FastVMState fast_state;
  fast_state.reststate = state;
  fast_state.root = state->root;
  fast_state.cf = &state->stack_ptr[state->stack_len - 1];
  fast_state.instr = fast_state.cf->instr_ptr;
  VMInstrFn fn = (VMInstrFn) instr_fns[fast_state.instr->type];
  int i;
  for (i = 0; i < 128 && fn != vm_halt; i++) {
    fn = fn(&fast_state).self;
    fn = fn(&fast_state).self;
    fn = fn(&fast_state).self;
    fn = fn(&fast_state).self;
    fn = fn(&fast_state).self;
    fn = fn(&fast_state).self;
    fn = fn(&fast_state).self;
    fn = fn(&fast_state).self;
    fn = fn(&fast_state).self;
  }
  state->shared->cyclecount += i * 9;
  vm_record_profile(state);
  state->stack_ptr[state->stack_len - 1].instr_ptr = fast_state.instr;
  fast_state.cf->instr_ptr = fast_state.instr;
}

void init_instr_fn_table() {
  instr_fns[INSTR_GET_ROOT] = vm_instr_get_root;
  instr_fns[INSTR_GET_CONTEXT] = vm_instr_get_context;
  instr_fns[INSTR_ALLOC_OBJECT] = vm_instr_alloc_object;
  instr_fns[INSTR_ALLOC_INT_OBJECT] = vm_instr_alloc_int_object;
  instr_fns[INSTR_ALLOC_FLOAT_OBJECT] = vm_instr_alloc_float_object;
  instr_fns[INSTR_ALLOC_ARRAY_OBJECT] = vm_instr_alloc_array_object;
  instr_fns[INSTR_ALLOC_STRING_OBJECT] = vm_instr_alloc_string_object;
  instr_fns[INSTR_ALLOC_CLOSURE_OBJECT] = vm_instr_alloc_closure_object;
  instr_fns[INSTR_CLOSE_OBJECT] = vm_instr_close_object;
  instr_fns[INSTR_FREEZE_OBJECT] = vm_instr_freeze_object;
  instr_fns[INSTR_ACCESS] = vm_instr_access;
  instr_fns[INSTR_ASSIGN] = vm_instr_assign;
  instr_fns[INSTR_CALL] = vm_instr_call;
  instr_fns[INSTR_RETURN] = vm_instr_return;
  instr_fns[INSTR_SAVE_RESULT] = vm_instr_save_result;
  instr_fns[INSTR_BR] = vm_instr_br;
  instr_fns[INSTR_TESTBR] = vm_instr_testbr;
  instr_fns[INSTR_ACCESS_STRING_KEY] = vm_instr_access_string_key;
  instr_fns[INSTR_ASSIGN_STRING_KEY] = vm_instr_assign_string_key;
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
    if (state->stack_len == 0) {
      state->runstate = VM_TERMINATED;
      break;
    }
  }
  gc_remove_roots(state, &result_set);
}
