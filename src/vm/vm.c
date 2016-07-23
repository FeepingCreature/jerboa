#include "vm/vm.h"

#include <stdarg.h>
#include <stdio.h>

#include "vm/call.h"
#include "vm/dump.h"
#include "gc.h"

const long long sample_stepsize = 200000LL; // 0.2ms

static void *vm_stack_alloc_internal(VMState *state, int size) {
  VMSharedState *shared = state->shared;
  if (UNLIKELY(shared->stack_data_len == 0)) {
    shared->stack_data_len = 16*1024*1024;
    shared->stack_data_ptr = malloc(shared->stack_data_len);
  }
  int new_offset = shared->stack_data_offset + size;
  if (UNLIKELY(new_offset > shared->stack_data_len)) {
    vm_error(state, "VM stack overflow!");
    return NULL;
  }
  void *ptr = (char*) shared->stack_data_ptr + shared->stack_data_offset;
  shared->stack_data_offset = new_offset;
  return ptr;
}

void *vm_stack_alloc(VMState *state, int size) {
  void *res = vm_stack_alloc_internal(state, size);
  bzero(res, size);
  return res;
}

void *vm_stack_alloc_uninitialized(VMState *state, int size) {
  return vm_stack_alloc_internal(state, size);
}

void vm_stack_free(VMState *state, void *ptr, int size) {
  VMSharedState *shared = state->shared;
  int new_offset = shared->stack_data_offset - size;
  (void) ptr; assert(ptr == (char*) shared->stack_data_ptr + new_offset); // must free in reverse order!
  shared->stack_data_offset = new_offset;
}

void vm_alloc_frame(VMState *state, int slots, int refslots) {
  Callframe *cf = (Callframe*) vm_stack_alloc(state, sizeof(Callframe));
  if (!cf) return; // stack overflow
  cf->above = state->frame;
  cf->slots_len = slots;
  cf->slots_ptr = vm_stack_alloc(state, sizeof(Value) * slots);
  if (!cf->slots_ptr) { // stack overflow
    vm_stack_free(state, cf, sizeof(Callframe));
    return;
  }
  
  cf->refslots_len = refslots;
  cf->refslots_ptr = vm_stack_alloc_uninitialized(state, sizeof(Value*) * refslots);
  if (!cf->refslots_ptr) { // stack overflow
    vm_stack_free(state, cf->slots_ptr, sizeof(Value) * slots);
    vm_stack_free(state, cf, sizeof(Callframe));
    return;
  }
  // no need to zero refslots, as they're not gc'd
  
  state->frame = cf;
}

void vm_error(VMState *state, const char *fmt, ...) {
  assert(state->runstate != VM_ERRORED);
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
  Callframe *cf = state->frame;
  vm_stack_free(state, cf->refslots_ptr, sizeof(Value*)*cf->refslots_len);
  vm_stack_free(state, cf->slots_ptr, sizeof(Value)*cf->slots_len);
  state->frame = cf->above;
  if (state->frame) state->frame->backtrace_belongs_to_p = NULL; // we're the head frame again, use instr_ptr again
  vm_stack_free(state, cf, sizeof(Callframe));
}

void vm_print_backtrace(VMState *state) {
  int k = state->backtrace_depth;
  if (state->backtrace) fprintf(stderr, "%s", state->backtrace);
  while (state) {
    for (Callframe *curf = state->frame; curf; k++, curf = curf->above) {
      Instr *instr = curf->instr_ptr;
      FileRange *belongs_to;
      if (curf->backtrace_belongs_to_p) belongs_to = *curf->backtrace_belongs_to_p;
      else belongs_to = instr->belongs_to;
      
      const char *file;
      TextRange line;
      int row, col1, col2;
      bool found = find_text_pos(belongs_to->text_from, &file, &line, &row, &col1);
      (void) found;
      assert(found);
      found = find_text_pos(belongs_to->text_to, &file, &line, &row, &col2);
      assert(found);
      int len = (int) (line.end - line.start - 1);
      if (col1 > len) col1 = len;
      if (col2 > len) col2 = len;
      // if (strcmp(file, "test4.jb") == 0 && row == 18) __asm__("int $3");
      fprintf(stderr, "#%i\t%s:%i\t", k+1, file, row+1);
      fprintf(stderr, "%.*s", col1, line.start);
      fprintf(stderr, "\x1b[1m%.*s\x1b[0m", col2 - col1, line.start + col1);
      fprintf(stderr, "%.*s\n", len - col2, line.start + col2);
    }
    state = state->parent;
  }
}

char *vm_record_backtrace(VMState *state, int *depth) {
  char *res_ptr = NULL; int res_len = 0;
  int k = state->backtrace_depth;
  if (state->backtrace) {
    res_len = strlen(state->backtrace);
    res_ptr = malloc(res_len + 1);
    strncpy(res_ptr, state->backtrace, res_len + 1);
  } else res_ptr = malloc(1);
  for (Callframe *curf = state->frame; curf; k++, curf = curf->above) {
    Instr *instr = curf->instr_ptr;
    FileRange *belongs_to;
    if (curf->backtrace_belongs_to_p) belongs_to = *curf->backtrace_belongs_to_p;
    else belongs_to = instr->belongs_to;
    
    const char *file;
    TextRange line;
    int row, col;
    bool found = find_text_pos(belongs_to->text_from, &file, &line, &row, &col);
    (void) found; assert(found);
    int size = snprintf(NULL, 0, "#%i\t%s:%i\t%.*s\n", k+1, file, row+1, (int) (line.end - line.start - 1), line.start);
    res_ptr = realloc(res_ptr, res_len + size + 1);
    snprintf(res_ptr + res_len, size + 1, "#%i\t%s:%i\t%.*s\n", k+1, file, row+1, (int) (line.end - line.start - 1), line.start);
    res_len += size;
  }
  res_ptr[res_len] = 0;
  *depth = k;
  return res_ptr;
}

void vm_record_profile(VMState *state) {
  int cyclecount = state->shared->cyclecount;
  HashTable *direct_tbl = &state->shared->profstate.direct_table;
  HashTable *indirect_tbl = &state->shared->profstate.indirect_table;
  
  VMState *curstate = state;
  // fprintf(stderr, "generate backtrace\n");
  // fprintf(stderr, "------\n");
  // vm_print_backtrace(state);
  int k = 0;
  while (curstate) {
    for (Callframe *curf = curstate->frame; curf; k++, curf = curf->above) {
      Instr *instr = curf->instr_ptr;
      FileRange **belongs_to_p = curf->backtrace_belongs_to_p;
      if (!belongs_to_p) belongs_to_p = &instr->belongs_to;
      
      // ranges are unique (and instrs must live as long as the vm state lives anyways)
      // so we can just use the pointer stored in the instr as the key
      char *key_ptr = (char*) belongs_to_p;
      int key_len = sizeof(*belongs_to_p);
      
      size_t key_hash = hash(key_ptr, key_len);
      
      if (k == 0) {
        TableEntry *freeptr;
        TableEntry *entry_p = table_lookup_alloc_with_hash(direct_tbl, key_ptr, key_len, key_hash, &freeptr);
        if (entry_p) entry_p->value.i ++;
        else freeptr->value = INT2VAL(1);
      } else {
        // don't double-count ranges in case of recursion
        bool range_already_counted = (*belongs_to_p)->last_cycle_seen == cyclecount;
        
        if (!range_already_counted) {
          TableEntry *freeptr;
          TableEntry *entry_p = table_lookup_alloc_with_hash(indirect_tbl, key_ptr, key_len, key_hash, &freeptr);
          if (entry_p) entry_p->value.i ++;
          else freeptr->value = INT2VAL(1);
        }
      }
      (*belongs_to_p)->last_cycle_seen = cyclecount;
    }
    curstate = curstate->parent;
  }
}

int num_msg_printed = 0;

void vm_maybe_record_profile(VMState *state) {
  struct timespec prof_time;
  long long ns_diff = get_clock_and_difference(&prof_time, &state->shared->profstate.last_prof_time);
  if (ns_diff > sample_stepsize) {
    state->shared->profstate.last_prof_time = prof_time;
    vm_record_profile(state);
    long long ns_taken = get_clock_and_difference(NULL, &prof_time);
    if (num_msg_printed <= 10 && ns_taken > sample_stepsize / 3) {
      fprintf(stderr, "warning: collecting profiling info took %lli%% of the last step.\n", ns_taken * 100LL / sample_stepsize);
      num_msg_printed++;
      if (num_msg_printed > 10) {
        fprintf(stderr, "warning will not be shown again.\n");
      }
    }
  }
}

typedef struct {
  Object *root;
  Callframe *cf;
  Instr *instr;
  Value *slots;
  
  VMState *reststate;
} FastVMState;

static void faststate_refresh(FastVMState *state) {
  state->cf = state->reststate->frame;
  state->instr = state->cf->instr_ptr;
  state->slots = state->cf->slots_ptr;
}

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
  state->slots[slot] = OBJ2VAL(state->root);
  state->instr = (Instr*)(get_root_instr + 1);
  return (FnWrap) { instr_fns[state->instr->type] };
}

static FnWrap vm_instr_alloc_object(FastVMState *state) {
  AllocObjectInstr *alloc_obj_instr = (AllocObjectInstr*) state->instr;
  int target_slot = alloc_obj_instr->target_slot, parent_slot = alloc_obj_instr->parent_slot;
  VM_ASSERT2_SLOT(target_slot < state->cf->slots_len, "slot numbering error");
  VM_ASSERT2_SLOT(parent_slot < state->cf->slots_len, "slot numbering error");
  Object *parent_obj = OBJ_OR_NULL(state->slots[parent_slot]);
  VM_ASSERT2(!parent_obj || !(parent_obj->flags & OBJ_NOINHERIT), "cannot inherit from object marked no-inherit");
  state->slots[target_slot] = make_object(state->reststate, parent_obj);
  state->instr = (Instr*)(alloc_obj_instr + 1);
  return (FnWrap) { instr_fns[state->instr->type] };
}

static FnWrap vm_instr_alloc_int_object(FastVMState *state) {
  AllocIntObjectInstr *alloc_int_obj_instr = (AllocIntObjectInstr*) state->instr;
  int target_slot = alloc_int_obj_instr->target_slot, value = alloc_int_obj_instr->value;
  VM_ASSERT2_SLOT(target_slot < state->cf->slots_len, "slot numbering error");
  state->slots[target_slot] = INT2VAL(value);
  state->instr = (Instr*)(alloc_int_obj_instr + 1);
  return (FnWrap) { instr_fns[state->instr->type] };
}

static FnWrap vm_instr_alloc_bool_object(FastVMState *state) {
  AllocBoolObjectInstr *alloc_bool_obj_instr = (AllocBoolObjectInstr*) state->instr;
  int target_slot = alloc_bool_obj_instr->target_slot;
  bool value = alloc_bool_obj_instr->value;
  VM_ASSERT2_SLOT(target_slot < state->cf->slots_len, "slot numbering error");
  state->slots[target_slot] = BOOL2VAL(value);
  state->instr = (Instr*)(alloc_bool_obj_instr + 1);
  return (FnWrap) { instr_fns[state->instr->type] };
}

static FnWrap vm_instr_alloc_float_object(FastVMState *state) {
  AllocFloatObjectInstr *alloc_float_obj_instr = (AllocFloatObjectInstr*) state->instr;
  int target_slot = alloc_float_obj_instr->target_slot; float value = alloc_float_obj_instr->value;
  VM_ASSERT2_SLOT(target_slot < state->cf->slots_len, "slot numbering error");
  state->slots[target_slot] = FLOAT2VAL(value);
  state->instr = (Instr*)(alloc_float_obj_instr + 1);
  return (FnWrap) { instr_fns[state->instr->type] };
}

static FnWrap vm_instr_alloc_array_object(FastVMState *state) {
  AllocArrayObjectInstr *alloc_array_obj_instr = (AllocArrayObjectInstr*) state->instr;
  int target_slot = alloc_array_obj_instr->target_slot;
  VM_ASSERT2_SLOT(target_slot < state->cf->slots_len, "slot numbering error");
  state->slots[target_slot] = make_array(state->reststate, NULL, 0);
  state->instr = (Instr*)(alloc_array_obj_instr + 1);
  return (FnWrap) { instr_fns[state->instr->type] };
}

static FnWrap vm_instr_alloc_string_object(FastVMState *state) {
  AllocStringObjectInstr *alloc_string_obj_instr = (AllocStringObjectInstr*) state->instr;
  int target_slot = alloc_string_obj_instr->target_slot; char *value = alloc_string_obj_instr->value;
  VM_ASSERT2_SLOT(target_slot < state->cf->slots_len, "slot numbering error");
  state->slots[target_slot] = make_string(state->reststate, value, strlen(value));
  state->instr = (Instr*)(alloc_string_obj_instr + 1);
  return (FnWrap) { instr_fns[state->instr->type] };
}

static FnWrap vm_instr_alloc_closure_object(FastVMState *state) {
  AllocClosureObjectInstr *alloc_closure_obj_instr = (AllocClosureObjectInstr*) state->instr;
  int target_slot = alloc_closure_obj_instr->target_slot;
  int context_slot = alloc_closure_obj_instr->base.context_slot;
  VM_ASSERT2_SLOT(target_slot < state->cf->slots_len, "slot numbering error");
  VM_ASSERT2_SLOT(context_slot < state->cf->slots_len, "slot numbering error");
  Value context = state->slots[context_slot];
  VM_ASSERT2(IS_OBJ(context), "bad slot type");
  Object *context_obj = AS_OBJ(context);
  state->slots[target_slot] = make_closure_fn(state->reststate, context_obj, alloc_closure_obj_instr->fn);
  state->instr = (Instr*)(alloc_closure_obj_instr + 1);
  return (FnWrap) { instr_fns[state->instr->type] };
}

static FnWrap vm_instr_close_object(FastVMState *state) {
  CloseObjectInstr *close_object_instr = (CloseObjectInstr*) state->instr;
  int slot = close_object_instr->slot;
  VM_ASSERT2_SLOT(slot < state->cf->slots_len, "slot numbering error");
  Value val = state->slots[slot];
  // non-object values are always OBJ_CLOSED
  VM_ASSERT2(IS_OBJ(val) && !(AS_OBJ(val)->flags & OBJ_CLOSED), "object is already closed!");
  AS_OBJ(val)->flags |= OBJ_CLOSED;
  state->instr = (Instr*)(close_object_instr + 1);
  return (FnWrap) { instr_fns[state->instr->type] };
}

static FnWrap vm_instr_freeze_object(FastVMState *state) {
  FreezeObjectInstr *freeze_object_instr = (FreezeObjectInstr*) state->instr;
  int slot = freeze_object_instr->slot;
  VM_ASSERT2_SLOT(slot < state->cf->slots_len, "slot numbering error");
  Value val = state->slots[slot];
  // non-object values are always OBJ_FROZEN
  VM_ASSERT2(IS_OBJ(val) && !(AS_OBJ(val)->flags & OBJ_FROZEN), "object is already frozen!");
  AS_OBJ(val)->flags |= OBJ_FROZEN;
  state->instr = (Instr*)(freeze_object_instr + 1);
  return (FnWrap) { instr_fns[state->instr->type] };
}

static FnWrap call_internal(FastVMState *state, int target_slot, Value this_val, Value fn_val, Value *args_ptr, int args_len, FileRange **prev_instr) {
  VM_ASSERT2(IS_OBJ(fn_val), "wrong type for function");
  // stackframe's instr_ptr will be pointed at the instr _after_ the call, but this messes up backtraces
  // solve explicitly
  state->cf->backtrace_belongs_to_p = prev_instr;
  state->cf->target_slot = &state->cf->slots_ptr[target_slot];
  state->cf->instr_ptr = state->instr;
  if (!setup_call(state->reststate, this_val, fn_val, args_ptr, args_len)) return (FnWrap) { vm_halt };
  
  // intrinsic may have errored.
  if (state->reststate->runstate == VM_ERRORED) return (FnWrap) { vm_halt };
  
  faststate_refresh(state);
  return (FnWrap) { instr_fns[state->instr->type] };
}

static FnWrap vm_instr_access(FastVMState *state) {
  AccessInstr *access_instr = (AccessInstr*) state->instr;
  int obj_slot, target_slot;
  obj_slot = access_instr->obj_slot;
  target_slot = access_instr->target_slot;
  
  VM_ASSERT2_SLOT(obj_slot < state->cf->slots_len, "internal slot error");
  VM_ASSERT2_SLOT(target_slot < state->cf->slots_len, "internal slot error");
  Value val = state->slots[obj_slot];
  Object *obj = closest_obj(state->reststate, val);
  
  char *key;
  bool has_char_key = false;
      
  int key_slot = access_instr->key_slot;
  VM_ASSERT2_SLOT(key_slot < state->cf->slots_len, "internal slot error");
  VM_ASSERT2(NOT_NULL(state->slots[key_slot]), "key slot null"); // TODO "slot_assigned"
  Object *string_base = state->reststate->shared->vcache.string_base;
  Value key_val = state->slots[key_slot];
  VM_ASSERT2(NOT_NULL(key_val), "key is null");
  Object *key_obj = OBJ_OR_NULL(key_val);
  StringObject *skey = (StringObject*) obj_instance_of(key_obj, string_base);
  bool object_found = false;
  if (skey) {
    key_obj->flags |= OBJ_IMMORTAL; // TODO better way
    key = skey->value;
    state->slots[target_slot] = object_lookup(obj, key, &object_found);
  }
  if (!object_found) {
    Value index_op = OBJECT_LOOKUP_STRING(obj, "[]", NULL);
    if (NOT_NULL(index_op)) {
      FileRange **prev_instr = &state->instr->belongs_to;
      state->instr = (Instr*)(access_instr + 1);
      return call_internal(state, target_slot, val, index_op, &key_val, 1, prev_instr);
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

static FnWrap vm_instr_access_string_key_index_fallback(FastVMState *state) {
  AccessStringKeyInstr *aski = (AccessStringKeyInstr*) state->instr;
  Value val = state->slots[aski->obj_slot];
  Object *obj = closest_obj(state->reststate, val);
  Value index_op = OBJECT_LOOKUP_STRING(obj, "[]", NULL);
  if (NOT_NULL(index_op)) {
    Value key = make_string(state->reststate, aski->key_ptr, aski->key_len);
    
    FileRange **prev_instr = &state->instr->belongs_to;
    state->instr = (Instr*)(aski + 1);
    return call_internal(state, aski->target_slot, val, index_op, &key, 1, prev_instr);
  } else {
    VM_ASSERT2(false, "property not found: '%.*s'", aski->key_len, aski->key_ptr);
  }
}

static FnWrap vm_instr_access_string_key(FastVMState *state) __attribute__ ((hot));
static FnWrap vm_instr_access_string_key(FastVMState *state) {
  AccessStringKeyInstr *aski = (AccessStringKeyInstr*) state->instr;
  int obj_slot = aski->obj_slot;
  int target_slot = aski->target_slot;
  
  VM_ASSERT2_SLOT(obj_slot < state->cf->slots_len, "internal slot error");
  VM_ASSERT2_SLOT(target_slot < state->cf->slots_len, "internal slot error");
  Object *obj = closest_obj(state->reststate, state->slots[obj_slot]);
  
  char *key_ptr = aski->key_ptr;
  int key_len = aski->key_len;
  size_t key_hash = aski->key_hash;
  bool object_found = false;
  state->slots[target_slot] = object_lookup_with_hash(obj, key_ptr, key_len, key_hash, &object_found);
  
  if (UNLIKELY(!object_found)) {
    return vm_instr_access_string_key_index_fallback(state);
  }
  
  state->instr = (Instr*)(aski + 1);
  return (FnWrap) { instr_fns[state->instr->type] };
}

static FnWrap vm_instr_assign(FastVMState *state) {
  AssignInstr *assign_instr = (AssignInstr*) state->instr;
  int obj_slot = assign_instr->obj_slot, value_slot = assign_instr->value_slot;
  int key_slot = assign_instr->key_slot, target_slot = assign_instr->target_slot;
  VM_ASSERT2_SLOT(obj_slot < state->cf->slots_len, "slot numbering error");
  VM_ASSERT2_SLOT(value_slot < state->cf->slots_len, "slot numbering error");
  VM_ASSERT2_SLOT(key_slot < state->cf->slots_len, "slot numbering error");
  VM_ASSERT2_SLOT(target_slot < state->cf->slots_len, "slot numbering error");
  VM_ASSERT2(NOT_NULL(state->slots[key_slot]), "key slot null"); // TODO see above
  Value val = state->slots[obj_slot];
  Object *obj = closest_obj(state->reststate, val);
  Value value_val = state->slots[value_slot];
  Object *string_base = state->reststate->shared->vcache.string_base;
  Value key_val = state->slots[key_slot];
  StringObject *skey = (StringObject*) obj_instance_of(OBJ_OR_NULL(key_val), string_base);
  if (!skey) {
    // non-string key, goes to []=
    Value index_assign_op = OBJECT_LOOKUP_STRING(obj, "[]=", NULL);
    if (NOT_NULL(index_assign_op)) {
      Value key_value_pair[] = {state->slots[assign_instr->key_slot], value_val};
      
      FileRange **prev_instr = &state->instr->belongs_to;
      state->instr = (Instr*)(assign_instr + 1);
      return call_internal(state, target_slot, val, index_assign_op, key_value_pair, 2, prev_instr);
    }
    VM_ASSERT2(false, "key is not string and no '[]=' is set");
  }
  char *key = my_asprintf("%s", skey->value); // copy so key can get gc'd without problems
  AssignType assign_type = assign_instr->type;
  // fprintf(stderr, "> obj set %p . '%s' = %p\n", (void*) obj, key, (void*) value_obj);
  VM_ASSERT2(obj, "assignment to null object");
  switch (assign_type) {
    case ASSIGN_PLAIN:
    {
      char *error = object_set(state->reststate, obj, key, value_val);
      VM_ASSERT2(!error, error);
      break;
    }
    case ASSIGN_EXISTING:
    {
      char *error = object_set_existing(state->reststate, obj, key, val);
      VM_ASSERT2(!error, error);
      break;
    }
    case ASSIGN_SHADOWING:
    {
      bool key_set;
      char *error = object_set_shadowing(state->reststate, obj, key, val, &key_set);
      VM_ASSERT2(error == NULL, "while assigning: %s", error);
      VM_ASSERT2(key_set, "key '%s' not found in object", key);
      break;
    }
  }
  state->instr = (Instr*)(assign_instr + 1);
  return (FnWrap) { instr_fns[state->instr->type] };
}

static FnWrap vm_instr_key_in_obj(FastVMState *state) {
  KeyInObjInstr *key_in_obj_instr = (KeyInObjInstr*) state->instr;
  int key_slot = key_in_obj_instr->key_slot, obj_slot = key_in_obj_instr->obj_slot;
  int target_slot = key_in_obj_instr->target_slot;
  VM_ASSERT2_SLOT(key_slot < state->cf->slots_len, "slot numbering error");
  VM_ASSERT2_SLOT(obj_slot < state->cf->slots_len, "slot numbering error");
  VM_ASSERT2_SLOT(target_slot < state->cf->slots_len, "slot numbering error");
  Object *obj = closest_obj(state->reststate, state->slots[obj_slot]);
  Object *string_base = state->reststate->shared->vcache.string_base;
  Value key_val = state->slots[key_slot];
  VM_ASSERT2(NOT_NULL(key_val), "key is null");
  StringObject *skey = (StringObject*) obj_instance_of(OBJ_OR_NULL(key_val), string_base);
  if (!skey) {
    VM_ASSERT2(false, "'in' key is not string! todo overload?");
  }
  char *key = skey->value;
  bool object_found = false;
  object_lookup(obj, key, &object_found);
  state->slots[target_slot] = BOOL2VAL(object_found);
  
  state->instr = (Instr*)(key_in_obj_instr + 1);
  return (FnWrap) { instr_fns[state->instr->type] };
}

static FnWrap vm_instr_instanceof(FastVMState *state) {
  InstanceofInstr *instr = (InstanceofInstr*) state->instr;
  int obj_slot = instr->obj_slot, proto_slot = instr->proto_slot;
  int target_slot = instr->target_slot;
  VM_ASSERT2_SLOT(obj_slot < state->cf->slots_len, "slot numbering error");
  VM_ASSERT2_SLOT(proto_slot < state->cf->slots_len, "slot numbering error");
  VM_ASSERT2_SLOT(target_slot < state->cf->slots_len, "slot numbering error");
  
  Value proto_val = state->slots[proto_slot];
  VM_ASSERT2(NOT_NULL(proto_val), "bad argument: instanceof null");
  bool res;
  if (!IS_OBJ(proto_val)) res = false; // nothing is instanceof 5
  else res = value_instance_of(state->reststate, state->slots[obj_slot], AS_OBJ(proto_val));
  state->slots[target_slot] = BOOL2VAL(res);
  
  state->instr = (Instr*)(instr + 1);
  return (FnWrap) { instr_fns[state->instr->type] };
}

static FnWrap vm_instr_set_constraint(FastVMState *state) {
  SetConstraintInstr *set_constraint_instr = (SetConstraintInstr*) state->instr;
  int key_slot = set_constraint_instr->key_slot, obj_slot = set_constraint_instr->obj_slot;
  int cons_slot = set_constraint_instr->constraint_slot;
  VM_ASSERT2_SLOT(key_slot < state->cf->slots_len, "slot numbering error");
  VM_ASSERT2_SLOT(obj_slot < state->cf->slots_len, "slot numbering error");
  VM_ASSERT2_SLOT(cons_slot < state->cf->slots_len, "slot numbering error");
  Value val = state->slots[obj_slot];
  VM_ASSERT2(NOT_NULL(val), "can't set constraint on null");
  VM_ASSERT2(IS_OBJ(val), "can't set constraint on primitive");
  Object *obj = AS_OBJ(val);
  Value constraint_val = state->slots[cons_slot];
  VM_ASSERT2(IS_OBJ(constraint_val), "constraint must not be primitive!");
  Object *constraint = AS_OBJ(constraint_val);
  Object *string_base = state->reststate->shared->vcache.string_base;
  Value key_val = state->slots[key_slot];
  StringObject *skey = (StringObject*) obj_instance_of(OBJ_OR_NULL(key_val), string_base);
  VM_ASSERT2(skey, "constraint key must be string");
  char *key = skey->value;
  
  char *error = object_set_constraint(state->reststate, obj, key, strlen(key), constraint);
  VM_ASSERT2(!error, "error while setting constraint: %s", error);
  
  state->instr = (Instr*)(set_constraint_instr + 1);
  return (FnWrap) { instr_fns[state->instr->type] };
}

static FnWrap vm_instr_assign_string_key(FastVMState *state) {
  AssignStringKeyInstr *aski = (AssignStringKeyInstr*) state->instr;
  int obj_slot = aski->obj_slot, value_slot = aski->value_slot;
  VM_ASSERT2_SLOT(obj_slot < state->cf->slots_len, "slot numbering error");
  VM_ASSERT2_SLOT(value_slot < state->cf->slots_len, "slot numbering error");
  Value obj_val = state->slots[obj_slot];
  Value value = state->slots[value_slot];
  char *key = aski->key;
  AssignType assign_type = aski->type;
  VM_ASSERT2(NOT_NULL(obj_val), "assignment to null");
  switch (assign_type) {
    case ASSIGN_PLAIN:
    {
      VM_ASSERT2(IS_OBJ(obj_val), "can't assign to primitive");
      char *error = object_set(state->reststate, AS_OBJ(obj_val), key, value);
      VM_ASSERT2(!error, error);
      break;
    }
    case ASSIGN_EXISTING:
    {
      Object *obj = closest_obj(state->reststate, obj_val);
      char *error = object_set_existing(state->reststate, obj, key, value);
      VM_ASSERT2(!error, error);
      break;
    }
    case ASSIGN_SHADOWING:
    {
      VM_ASSERT2(IS_OBJ(obj_val), "can't assign to primitive");
      bool key_set;
      char *error = object_set_shadowing(state->reststate, AS_OBJ(obj_val), key, value, &key_set);
      VM_ASSERT2(error == NULL, "while assigning '%s': %s", key, error);
      VM_ASSERT2(key_set, "key '%s' not found in object", key);
      break;
    }
  }
  state->instr = (Instr*)(aski + 1);
  return (FnWrap) { instr_fns[state->instr->type] };
}

static FnWrap vm_instr_set_constraint_string_key(FastVMState *state) {
  SetConstraintStringKeyInstr *scski = (SetConstraintStringKeyInstr*) state->instr;
  int obj_slot = scski->obj_slot, cons_slot = scski->constraint_slot;
  VM_ASSERT2_SLOT(obj_slot < state->cf->slots_len, "slot numbering error");
  VM_ASSERT2_SLOT(cons_slot < state->cf->slots_len, "slot numbering error");
  Value val = state->slots[obj_slot];
  VM_ASSERT2(NOT_NULL(val), "can't set constraint on null");
  VM_ASSERT2(IS_OBJ(val), "can't set constraint on primitive");
  Object *obj = AS_OBJ(val);
  Value constraint_val = state->slots[cons_slot];
  VM_ASSERT2(IS_OBJ(constraint_val), "constraint must not be primitive!");
  Object *constraint = AS_OBJ(constraint_val);
  
  char *error = object_set_constraint(state->reststate, obj, scski->key_ptr, scski->key_len, constraint);
  VM_ASSERT2(!error, error);
  
  state->instr = (Instr*)(scski + 1);
  return (FnWrap) { instr_fns[state->instr->type] };
}

static FnWrap vm_instr_call(FastVMState *state) {
  CallInstr *call_instr = (CallInstr*) state->instr;
  int this_slot = call_instr->this_slot, args_length = call_instr->args_length;
  int function_slot = call_instr->function_slot, target_slot = call_instr->target_slot;
  VM_ASSERT2_SLOT(function_slot < state->cf->slots_len, "slot numbering error");
  VM_ASSERT2_SLOT(target_slot < state->cf->slots_len, "slot numbering error");
  VM_ASSERT2_SLOT(this_slot < state->cf->slots_len, "slot numbering error");
  Value this_val = state->slots[this_slot];
  Value fn_val = state->slots[function_slot];
  
  // form args array from slots
  Value *args = alloca(sizeof(Value) * args_length);
  
  for (int i = 0; i < args_length; ++i) {
    int argslot = ((int*)(call_instr + 1))[i];
    VM_ASSERT2_SLOT(argslot < state->cf->slots_len, "slot numbering error");
    args[i] = state->slots[argslot];
  }
  
  FileRange **prev_instr = &state->instr->belongs_to;
  state->instr = (Instr*) ((int*)(call_instr + 1) + call_instr->args_length);
  return call_internal(state, target_slot, this_val, fn_val, args, args_length, prev_instr);
}

static FnWrap vm_halt(FastVMState *state) {
  (void) state;
  return (FnWrap) { vm_halt };
}

static FnWrap vm_instr_return(FastVMState *state) {
  ReturnInstr *ret_instr = (ReturnInstr*) state->instr;
  int ret_slot = ret_instr->ret_slot;
  VM_ASSERT2_SLOT(ret_slot < state->cf->slots_len, "slot numbering error");
  Value res = state->slots[ret_slot];
  gc_remove_roots(state->reststate, &state->cf->frameroot_slots);
  vm_remove_frame(state->reststate);
  
  if (state->reststate->frame) {
    faststate_refresh(state);
    *state->cf->target_slot = res;
  } else {
    state->reststate->exit_value = res;
    return (FnWrap) { vm_halt };
  }
  
  return (FnWrap) { instr_fns[state->instr->type] };
}

static FnWrap vm_instr_br(FastVMState *state) {
  BranchInstr *br_instr = (BranchInstr*) state->instr;
  int blk = br_instr->blk;
  VM_ASSERT2_SLOT(blk < state->cf->uf->body.blocks_len, "slot numbering error");
  state->instr = (Instr*) ((char*) state->cf->uf->body.instrs_ptr + state->cf->uf->body.blocks_ptr[blk].offset);
  state->cf->prev_block = state->cf->block;
  state->cf->block = blk;
  return (FnWrap) { instr_fns[state->instr->type] };
}

static FnWrap vm_instr_testbr(FastVMState *state) {
  TestBranchInstr *tbr_instr = (TestBranchInstr*) state->instr;
  int test_slot = tbr_instr->test_slot;
  int true_blk = tbr_instr->true_blk, false_blk = tbr_instr->false_blk;
  VM_ASSERT2_SLOT(test_slot < state->cf->slots_len, "slot numbering error");
  Value test_value = state->slots[test_slot];
  
  bool test = value_is_truthy(test_value);
  
  int target_blk = test ? true_blk : false_blk;
  state->instr = (Instr*) ((char*) state->cf->uf->body.instrs_ptr + state->cf->uf->body.blocks_ptr[target_blk].offset);
  state->cf->prev_block = state->cf->block;
  state->cf->block = target_blk;
  return (FnWrap) { instr_fns[state->instr->type] };
}

static FnWrap vm_instr_phi(FastVMState *state) {
  PhiInstr *phi = (PhiInstr*) state->instr;
  int slot1 = phi->slot1, slot2 = phi->slot2, target_slot = phi->target_slot;
  VM_ASSERT2_SLOT(slot1 < state->cf->slots_len, "slot_numbering error");
  VM_ASSERT2_SLOT(slot2 < state->cf->slots_len, "slot_numbering error");
  VM_ASSERT2_SLOT(target_slot < state->cf->slots_len, "slot_numbering error");
  if (state->cf->prev_block == phi->block1) {
    state->slots[target_slot] = state->slots[slot1];
  } else if (state->cf->prev_block == phi->block2) {
    state->slots[target_slot] = state->slots[slot2];
  } else VM_ASSERT2(false, "phi block error: arrived here from block not in list: [%i, %i], but came from %i",
                    phi->block1, phi->block2, state->cf->prev_block);
  
  state->instr = (Instr*)(phi + 1);
  return (FnWrap) { instr_fns[state->instr->type] };
}

static FnWrap vm_instr_set_slot(FastVMState *state) {
  SetSlotInstr *ssi = (SetSlotInstr*) state->instr;
  int target_slot = ssi->target_slot;
  VM_ASSERT2_SLOT(target_slot < state->cf->slots_len, "slot numbering error");
  state->slots[target_slot] = ssi->value;
  
  state->instr = (Instr*)(ssi + 1);
  return (FnWrap) { instr_fns[state->instr->type] };
}

static FnWrap vm_instr_define_refslot(FastVMState *state) {
  DefineRefslotInstr *dri = (DefineRefslotInstr*) state->instr;
  
  int target_refslot = dri->target_refslot;
  int obj_slot = dri->obj_slot;
  VM_ASSERT2_SLOT(obj_slot < state->cf->slots_len, "slot numbering error");
  
  Object *obj = closest_obj(state->reststate, state->slots[obj_slot]);
  VM_ASSERT2(obj, "cannot define refslot for null obj");
  
  Value *pp = object_lookup_ref_with_hash(obj, dri->key_ptr, dri->key_len, dri->key_hash);
  VM_ASSERT2(pp, "key not in object");
  state->cf->refslots_ptr[target_refslot] = pp;
  
  state->instr = (Instr*)(dri + 1);
  return (FnWrap) { instr_fns[state->instr->type] };
}

static FnWrap vm_instr_read_refslot(FastVMState *state) {
  ReadRefslotInstr *rri = (ReadRefslotInstr*) state->instr;
  
  int target_slot = rri->target_slot;
  int source_refslot = rri->source_refslot;
  VM_ASSERT2_SLOT(target_slot < state->cf->slots_len, "slot numbering error");
  VM_ASSERT2_SLOT(source_refslot < state->cf->refslots_len, "refslot numbering error");
  
  state->slots[target_slot] = *state->cf->refslots_ptr[source_refslot];
  
  state->instr = (Instr*)(rri + 1);
  return (FnWrap) { instr_fns[state->instr->type] };
}

static FnWrap vm_instr_write_refslot(FastVMState *state) {
  WriteRefslotInstr *wri = (WriteRefslotInstr*) state->instr;
  
  int target_refslot = wri->target_refslot;
  int source_slot = wri->source_slot;
  VM_ASSERT2_SLOT(target_refslot < state->cf->refslots_len, "refslot numbering error");
  VM_ASSERT2_SLOT(source_slot < state->cf->slots_len, "slot numbering error");
  
  *state->cf->refslots_ptr[target_refslot] = state->slots[source_slot];
  
  state->instr = (Instr*)(wri + 1);
  return (FnWrap) { instr_fns[state->instr->type] };
}

static FnWrap vm_instr_alloc_static_object(FastVMState *state) {
  AllocStaticObjectInstr *asoi = (AllocStaticObjectInstr*) state->instr;
  
  int target_slot = asoi->target_slot, parent_slot = asoi->parent_slot;
  VM_ASSERT2_SLOT(target_slot < state->cf->slots_len, "slot numbering error");
  VM_ASSERT2_SLOT(parent_slot < state->cf->slots_len, "slot numbering error");
  Object *parent_obj = OBJ_OR_NULL(state->slots[parent_slot]);
  VM_ASSERT2(!parent_obj || !(parent_obj->flags & OBJ_NOINHERIT), "cannot inherit from object marked no-inherit");
  Object *obj = AS_OBJ(make_object(state->reststate, parent_obj));
  
  // TODO table_clone
  obj->tbl = ((Object*)(asoi+1))->tbl;
  int tbl_len = sizeof(TableEntry) * obj->tbl.entries_num;
  obj->tbl.entries_ptr = cache_alloc(tbl_len);
  memcpy(obj->tbl.entries_ptr, ((Object*)(asoi+1))->tbl.entries_ptr, tbl_len);
  
  for (int i = 0; i < asoi->info_len; ++i) {
    StaticFieldInfo *info = &asoi->info_ptr[i];
    VM_ASSERT2_SLOT(info->slot < state->cf->slots_len, "slot numbering error");
    TableEntry *entry = &obj->tbl.entries_ptr[info->tbl_offset];
    // copied from sample, should be no need to allocate
    entry->value = state->slots[info->slot];
    if (info->constraint) {
      entry->constraint = info->constraint;
      if (!value_instance_of(state->reststate, entry->value, info->constraint)) {
        VM_ASSERT2(false, "type constraint violated on variable");
      }
    }
    state->cf->refslots_ptr[info->refslot] = &entry->value;
  }
  
  obj->flags = OBJ_CLOSED;
  
  state->slots[target_slot] = OBJ2VAL(obj);
  
  state->instr = (Instr*)((char*) asoi + sizeof(AllocStaticObjectInstr) + sizeof(Object));
  return (FnWrap) { instr_fns[state->instr->type] };
}

static void vm_step(VMState *state) {
  FastVMState fast_state;
  fast_state.reststate = state;
  fast_state.root = state->root;
  faststate_refresh(&fast_state);
  VMInstrFn fn = (VMInstrFn) instr_fns[fast_state.instr->type];
  int i;
  for (i = 0; i < 128 && fn != vm_halt; i++) {
    // { fprintf(stderr, "run "); Instr *instr = fast_state.instr; dump_instr(&instr); }
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
  if (state->frame) state->frame->instr_ptr = fast_state.instr;
  vm_maybe_record_profile(state);
}

void init_instr_fn_table() {
  instr_fns[INSTR_GET_ROOT] = vm_instr_get_root;
  instr_fns[INSTR_ALLOC_OBJECT] = vm_instr_alloc_object;
  instr_fns[INSTR_ALLOC_INT_OBJECT] = vm_instr_alloc_int_object;
  instr_fns[INSTR_ALLOC_BOOL_OBJECT] = vm_instr_alloc_bool_object;
  instr_fns[INSTR_ALLOC_FLOAT_OBJECT] = vm_instr_alloc_float_object;
  instr_fns[INSTR_ALLOC_ARRAY_OBJECT] = vm_instr_alloc_array_object;
  instr_fns[INSTR_ALLOC_STRING_OBJECT] = vm_instr_alloc_string_object;
  instr_fns[INSTR_ALLOC_CLOSURE_OBJECT] = vm_instr_alloc_closure_object;
  instr_fns[INSTR_CLOSE_OBJECT] = vm_instr_close_object;
  instr_fns[INSTR_FREEZE_OBJECT] = vm_instr_freeze_object;
  instr_fns[INSTR_ACCESS] = vm_instr_access;
  instr_fns[INSTR_ASSIGN] = vm_instr_assign;
  instr_fns[INSTR_KEY_IN_OBJ] = vm_instr_key_in_obj;
  instr_fns[INSTR_INSTANCEOF] = vm_instr_instanceof;
  instr_fns[INSTR_SET_CONSTRAINT] = vm_instr_set_constraint;
  instr_fns[INSTR_CALL] = vm_instr_call;
  instr_fns[INSTR_RETURN] = vm_instr_return;
  instr_fns[INSTR_BR] = vm_instr_br;
  instr_fns[INSTR_TESTBR] = vm_instr_testbr;
  instr_fns[INSTR_PHI] = vm_instr_phi;
  instr_fns[INSTR_ACCESS_STRING_KEY] = vm_instr_access_string_key;
  instr_fns[INSTR_ASSIGN_STRING_KEY] = vm_instr_assign_string_key;
  instr_fns[INSTR_SET_CONSTRAINT_STRING_KEY] = vm_instr_set_constraint_string_key;
  instr_fns[INSTR_SET_SLOT] = vm_instr_set_slot;
  instr_fns[INSTR_DEFINE_REFSLOT] = vm_instr_define_refslot;
  instr_fns[INSTR_READ_REFSLOT] = vm_instr_read_refslot;
  instr_fns[INSTR_WRITE_REFSLOT] = vm_instr_write_refslot;
  instr_fns[INSTR_ALLOC_STATIC_OBJECT] = vm_instr_alloc_static_object;
}

void vm_run(VMState *state) {
  assert(state->runstate == VM_TERMINATED || state->runstate == VM_ERRORED);
  // no call queued, no need to run
  // (this can happen when we executed a native function,
  //  expecting to set up a vm call)
  if (!state->frame) return;
  state->runstate = VM_RUNNING;
  state->error = NULL;
  while (state->runstate == VM_RUNNING) {
    vm_step(state);
    if (!state->frame) {
      state->runstate = VM_TERMINATED;
      break;
    }
    if (state->shared->gcstate.num_obj_allocated > state->shared->gcstate.next_gc_run) {
      // fprintf(stderr, "allocated %i, next_gc_run %i\n", state->shared->gcstate.num_obj_allocated, state->shared->gcstate.next_gc_run);
      gc_run(state);
      // run gc after 50% growth or 10000000 allocated or thereabouts
      state->shared->gcstate.next_gc_run = (int) (state->shared->gcstate.num_obj_allocated * 1.5) + (10000000/32); // don't even get out of bed for less than 10MB
      // fprintf(stderr, "left over %i, set next to %i\n", state->shared->gcstate.num_obj_allocated, state->shared->gcstate.next_gc_run);
    }
  }
}
