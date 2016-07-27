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
  Callframe *cf = (Callframe*) vm_stack_alloc_uninitialized(state, sizeof(Callframe));
  if (!cf) return; // stack overflow
  cf->above = state->frame;
  cf->backtrace_belongs_to_p = NULL;
  cf->block = 0;
  cf->slots_len = slots;
  cf->slots_ptr = vm_stack_alloc_uninitialized(state, sizeof(Value) * slots);
  if (!cf->slots_ptr) { // stack overflow
    vm_stack_free(state, cf, sizeof(Callframe));
    return;
  }
  for (int i = 0; i < slots; ++i) cf->slots_ptr[i].type = TYPE_NULL;
  
  cf->refslots_len = refslots;
  cf->refslots_ptr = vm_stack_alloc_uninitialized(state, sizeof(Value*) * refslots);
  if (!cf->refslots_ptr) { // stack overflow
    vm_stack_free(state, cf->slots_ptr, sizeof(Value) * slots);
    vm_stack_free(state, cf, sizeof(Callframe));
    return;
  }
  // no need to zero refslots, as they're not gc'd
  
  state->frame = cf;
  gc_add_roots(state, cf->slots_ptr, cf->slots_len, &cf->frameroot_slots);
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

void setup_stub_frame(VMState *state, int slots) {
  if (state->frame) state->frame->instr_ptr = state->instr;
  vm_alloc_frame(state, slots + 1, 0);
  ReturnInstr *ret = calloc(sizeof(ReturnInstr), 1);
  ret->base.type = INSTR_RETURN;
  ret->ret = (Arg) { .kind = ARG_SLOT, .slot = slots };
  state->instr = (Instr*) ret;
}

void vm_print_backtrace(VMState *state) {
  int k = state->backtrace_depth;
  if (state->backtrace) fprintf(stderr, "%s", state->backtrace);
  while (state) {
    if (state->frame) state->frame->instr_ptr = state->instr;
    for (Callframe *curf = state->frame; curf; k++, curf = curf->above) {
      Instr *instr = curf->instr_ptr;
      FileRange *belongs_to;
      if (curf->backtrace_belongs_to_p) belongs_to = *curf->backtrace_belongs_to_p;
      else belongs_to = instr->belongs_to;
      if (!belongs_to) continue; // stub frame
      
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
  if (state->frame) state->frame->instr_ptr = state->instr;
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
    if (curstate->frame) curstate->frame->instr_ptr = curstate->instr;
    for (Callframe *curf = curstate->frame; curf; k++, curf = curf->above) {
      Instr *instr = curf->instr_ptr;
      FileRange **belongs_to_p = curf->backtrace_belongs_to_p;
      if (!belongs_to_p) belongs_to_p = &instr->belongs_to;
      
      if (!*belongs_to_p) continue; // stub frames and the like
      
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

struct _FnWrap;
typedef struct _FnWrap FnWrap;

typedef FnWrap (*VMInstrFn)(VMState *state);
struct _FnWrap {
  VMInstrFn self;
};

static FnWrap vm_halt(VMState *state);

#define VM_ASSERT2(cond, ...) if (UNLIKELY(!(cond)) && (vm_error(state, __VA_ARGS__), true)) return (FnWrap) { vm_halt }

#ifndef NDEBUG
#define VM_ASSERT2_SLOT(cond, ...) VM_ASSERT2(cond, __VA_ARGS__)
#else
#define VM_ASSERT2_SLOT(cond, ...) (void) 0
#endif

static VMInstrFn instr_fns[INSTR_LAST] = {0};

static FnWrap vm_instr_get_root(VMState *state) {
  GetRootInstr *get_root_instr = (GetRootInstr*) state->instr;
  int slot = get_root_instr->slot;
  VM_ASSERT2_SLOT(slot < state->frame->slots_len, "internal slot error");
  state->frame->slots_ptr[slot] = OBJ2VAL(state->root);
  state->instr = (Instr*)(get_root_instr + 1);
  return (FnWrap) { instr_fns[state->instr->type] };
}

static FnWrap vm_instr_alloc_object(VMState *state) {
  AllocObjectInstr *alloc_obj_instr = (AllocObjectInstr*) state->instr;
  int target_slot = alloc_obj_instr->target_slot, parent_slot = alloc_obj_instr->parent_slot;
  VM_ASSERT2_SLOT(target_slot < state->frame->slots_len, "slot numbering error");
  VM_ASSERT2_SLOT(parent_slot < state->frame->slots_len, "slot numbering error");
  Object *parent_obj = OBJ_OR_NULL(state->frame->slots_ptr[parent_slot]);
  VM_ASSERT2(!parent_obj || !(parent_obj->flags & OBJ_NOINHERIT), "cannot inherit from object marked no-inherit");
  state->frame->slots_ptr[target_slot] = make_object(state, parent_obj);
  state->instr = (Instr*)(alloc_obj_instr + 1);
  return (FnWrap) { instr_fns[state->instr->type] };
}

static FnWrap vm_instr_alloc_int_object(VMState *state) {
  AllocIntObjectInstr *alloc_int_obj_instr = (AllocIntObjectInstr*) state->instr;
  int value = alloc_int_obj_instr->value;
  set_arg(state, alloc_int_obj_instr->target, INT2VAL(value));
  state->instr = (Instr*)(alloc_int_obj_instr + 1);
  return (FnWrap) { instr_fns[state->instr->type] };
}

static FnWrap vm_instr_alloc_bool_object(VMState *state) {
  AllocBoolObjectInstr *alloc_bool_obj_instr = (AllocBoolObjectInstr*) state->instr;
  bool value = alloc_bool_obj_instr->value;
  set_arg(state, alloc_bool_obj_instr->target, BOOL2VAL(value));
  state->instr = (Instr*)(alloc_bool_obj_instr + 1);
  return (FnWrap) { instr_fns[state->instr->type] };
}

static FnWrap vm_instr_alloc_float_object(VMState *state) {
  AllocFloatObjectInstr *alloc_float_obj_instr = (AllocFloatObjectInstr*) state->instr;
  float value = alloc_float_obj_instr->value;
  set_arg(state, alloc_float_obj_instr->target, FLOAT2VAL(value));
  state->instr = (Instr*)(alloc_float_obj_instr + 1);
  return (FnWrap) { instr_fns[state->instr->type] };
}

static FnWrap vm_instr_alloc_array_object(VMState *state) {
  AllocArrayObjectInstr *alloc_array_obj_instr = (AllocArrayObjectInstr*) state->instr;
  set_arg(state, alloc_array_obj_instr->target, make_array(state, NULL, 0, true));
  state->instr = (Instr*)(alloc_array_obj_instr + 1);
  return (FnWrap) { instr_fns[state->instr->type] };
}

static FnWrap vm_instr_alloc_string_object(VMState *state) {
  AllocStringObjectInstr *alloc_string_obj_instr = (AllocStringObjectInstr*) state->instr;
  char *value = alloc_string_obj_instr->value;
  set_arg(state, alloc_string_obj_instr->target, make_string_static(state, value));
  state->instr = (Instr*)(alloc_string_obj_instr + 1);
  return (FnWrap) { instr_fns[state->instr->type] };
}

static FnWrap vm_instr_alloc_closure_object(VMState *state) {
  AllocClosureObjectInstr *alloc_closure_obj_instr = (AllocClosureObjectInstr*) state->instr;
  int context_slot = alloc_closure_obj_instr->base.context_slot;
  VM_ASSERT2_SLOT(context_slot < state->frame->slots_len, "slot numbering error");
  Value context = state->frame->slots_ptr[context_slot];
  VM_ASSERT2(IS_OBJ(context), "bad slot type");
  Object *context_obj = AS_OBJ(context);
  set_arg(state, alloc_closure_obj_instr->target, make_closure_fn(state, context_obj, alloc_closure_obj_instr->fn));
  state->instr = (Instr*)(alloc_closure_obj_instr + 1);
  return (FnWrap) { instr_fns[state->instr->type] };
}

static FnWrap vm_instr_close_object(VMState *state) {
  CloseObjectInstr *close_object_instr = (CloseObjectInstr*) state->instr;
  int slot = close_object_instr->slot;
  VM_ASSERT2_SLOT(slot < state->frame->slots_len, "slot numbering error");
  Value val = state->frame->slots_ptr[slot];
  // non-object values are always OBJ_CLOSED
  VM_ASSERT2(IS_OBJ(val) && !(AS_OBJ(val)->flags & OBJ_CLOSED), "object is already closed!");
  AS_OBJ(val)->flags |= OBJ_CLOSED;
  state->instr = (Instr*)(close_object_instr + 1);
  return (FnWrap) { instr_fns[state->instr->type] };
}

static FnWrap vm_instr_freeze_object(VMState *state) {
  FreezeObjectInstr *freeze_object_instr = (FreezeObjectInstr*) state->instr;
  int slot = freeze_object_instr->slot;
  VM_ASSERT2_SLOT(slot < state->frame->slots_len, "slot numbering error");
  Value val = state->frame->slots_ptr[slot];
  // non-object values are always OBJ_FROZEN
  VM_ASSERT2(IS_OBJ(val) && !(AS_OBJ(val)->flags & OBJ_FROZEN), "object is already frozen!");
  AS_OBJ(val)->flags |= OBJ_FROZEN;
  state->instr = (Instr*)(freeze_object_instr + 1);
  return (FnWrap) { instr_fns[state->instr->type] };
}

static inline bool call_internal(VMState *state, CallInfo *info, FileRange **prev_instr) {
  // stackframe's instr_ptr will be pointed at the instr _after_ the call, but this messes up backtraces
  // solve explicitly
  state->frame->backtrace_belongs_to_p = prev_instr;
  state->frame->instr_ptr = state->instr;
  
  return setup_call(state, info);
}

static FnWrap vm_instr_access(VMState *state) {
  AccessInstr *access_instr = (AccessInstr*) state->instr;
  
  Value val = load_arg(state->frame, access_instr->obj);
  Object *obj = closest_obj(state, val);
  
  char *key;
  bool has_char_key = false;
      
  Value key_val = load_arg(state->frame, access_instr->key);
  VM_ASSERT2(NOT_NULL(key_val), "key is null");
  Object *string_base = state->shared->vcache.string_base;
  Object *key_obj = OBJ_OR_NULL(key_val);
  StringObject *skey = (StringObject*) obj_instance_of(key_obj, string_base);
  bool object_found = false;
  if (skey) {
    has_char_key = true;
    key = skey->value;
    // otherwise, skey->value is independent of skey
    set_arg(state, access_instr->target, object_lookup(obj, key, &object_found));
  }
  if (!object_found) {
    Value index_op = OBJECT_LOOKUP_STRING(obj, "[]", NULL);
    if (NOT_NULL(index_op)) {
      FileRange **prev_instr = &state->instr->belongs_to;
      state->instr = (Instr*)(access_instr + 1);
      CallInfo *info = alloca(sizeof(CallInfo) + sizeof(Arg));
      info->args_len = 1;
      info->this_arg = (Arg) { .kind = ARG_VALUE, .value = val };
      info->fn = (Arg) { .kind = ARG_VALUE, .value = index_op };
      info->target = access_instr->target;
      INFO_ARGS_PTR(info)[0] = access_instr->key;
      if (!call_internal(state, info, prev_instr)) return (FnWrap) { vm_halt };
      
      return (FnWrap) { instr_fns[state->instr->type] };
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

static bool vm_instr_access_string_key_index_fallback(VMState *state, AccessStringKeyInstr *aski) {
  Value val = load_arg(state->frame, aski->obj);
  Object *obj = closest_obj(state, val);
  Value index_op = OBJECT_LOOKUP_STRING(obj, "[]", NULL);
  if (NOT_NULL(index_op)) {
    Value key = make_string(state, aski->key_ptr, aski->key_len);
    state->frame->slots_ptr[aski->key_slot] = key;
    
    CallInfo *info = alloca(sizeof(CallInfo) + sizeof(Arg));
    info->args_len = 1;
    info->this_arg = (Arg) { .kind = ARG_VALUE, .value = val };
    info->fn = (Arg) { .kind = ARG_VALUE, .value = index_op };
    info->target = aski->target;
    INFO_ARGS_PTR(info)[0] = (Arg) { .kind = ARG_SLOT, .slot = aski->key_slot };
    
    state->instr = (Instr*)(aski + 1);
    FileRange **prev_instr = &state->instr->belongs_to;
    return call_internal(state, info, prev_instr);
  } else {
    VM_ASSERT(false, "property not found: '%.*s'", aski->key_len, aski->key_ptr) false;
  }
}

static FnWrap vm_instr_access_string_key(VMState *state) {
  AccessStringKeyInstr *aski = (AccessStringKeyInstr*) state->instr;
  
  Object *obj = closest_obj(state, load_arg(state->frame, aski->obj));
  
  char *key_ptr = aski->key_ptr;
  int key_len = aski->key_len;
  size_t key_hash = aski->key_hash;
  bool object_found = false;
  set_arg(state, aski->target, object_lookup_with_hash(obj, key_ptr, key_len, key_hash, &object_found));
  
  if (UNLIKELY(!object_found)) {
    if (!vm_instr_access_string_key_index_fallback(state, aski)) return (FnWrap) { vm_halt };
  } else {
    state->instr = (Instr*)(aski + 1);
  }
  
  return (FnWrap) { instr_fns[state->instr->type] };
}

static FnWrap vm_instr_assign(VMState *state) {
  AssignInstr *assign_instr = (AssignInstr*) state->instr;
  int target_slot = assign_instr->target_slot;
  VM_ASSERT2_SLOT(target_slot < state->frame->slots_len, "slot numbering error");
  VM_ASSERT2(NOT_NULL(load_arg(state->frame, assign_instr->key)), "key slot null"); // TODO see above
  Value obj_val = load_arg(state->frame, assign_instr->obj);
  Object *obj = closest_obj(state, obj_val);
  Value value_val = load_arg(state->frame, assign_instr->value);
  Object *string_base = state->shared->vcache.string_base;
  Value key_val = load_arg(state->frame, assign_instr->key);
  StringObject *skey = (StringObject*) obj_instance_of(OBJ_OR_NULL(key_val), string_base);
  if (!skey) {
    // non-string key, goes to []=
    Value index_assign_op = OBJECT_LOOKUP_STRING(obj, "[]=", NULL);
    if (NOT_NULL(index_assign_op)) {
      CallInfo *info = alloca(sizeof(CallInfo) + sizeof(Arg) * 2);
      info->args_len = 2;
      info->this_arg = (Arg) { .kind = ARG_VALUE, .value = obj_val };
      info->fn = (Arg) { .kind = ARG_VALUE, .value = index_assign_op };
      info->target = (WriteArg) { .kind = ARG_SLOT, .slot = target_slot };
      INFO_ARGS_PTR(info)[0] = assign_instr->key;
      INFO_ARGS_PTR(info)[1] = assign_instr->value;
      
      FileRange **prev_instr = &state->instr->belongs_to;
      state->instr = (Instr*)(assign_instr + 1);
      if (!call_internal(state, info, prev_instr)) return (FnWrap) { vm_halt };
      return (FnWrap) { instr_fns[state->instr->type] };
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
      char *error = object_set(state, obj, key, value_val);
      VM_ASSERT2(!error, error);
      break;
    }
    case ASSIGN_EXISTING:
    {
      char *error = object_set_existing(state, obj, key, value_val);
      VM_ASSERT2(!error, error);
      break;
    }
    case ASSIGN_SHADOWING:
    {
      bool key_set;
      char *error = object_set_shadowing(state, obj, key, value_val, &key_set);
      VM_ASSERT2(error == NULL, "while assigning: %s", error);
      VM_ASSERT2(key_set, "key '%s' not found in object", key);
      break;
    }
  }
  state->instr = (Instr*)(assign_instr + 1);
  return (FnWrap) { instr_fns[state->instr->type] };
}

static FnWrap vm_instr_key_in_obj(VMState *state) {
  KeyInObjInstr *key_in_obj_instr = (KeyInObjInstr*) state->instr;
  Object *obj = closest_obj(state, load_arg(state->frame, key_in_obj_instr->obj));
  Object *string_base = state->shared->vcache.string_base;
  Value key_val = load_arg(state->frame, key_in_obj_instr->key);
  VM_ASSERT2(NOT_NULL(key_val), "key is null");
  StringObject *skey = (StringObject*) obj_instance_of(OBJ_OR_NULL(key_val), string_base);
  if (!skey) {
    VM_ASSERT2(false, "'in' key is not string! todo overload?");
  }
  char *key = skey->value;
  bool object_found = false;
  object_lookup(obj, key, &object_found);
  set_arg(state, key_in_obj_instr->target, BOOL2VAL(object_found));
  
  state->instr = (Instr*)(key_in_obj_instr + 1);
  return (FnWrap) { instr_fns[state->instr->type] };
}

static FnWrap vm_instr_identical(VMState *state) {
  IdenticalInstr *instr= (IdenticalInstr*) state->instr;
  Value arg1 = load_arg(state->frame, instr->obj1);
  Value arg2 = load_arg(state->frame, instr->obj2);
  bool res;
  if (arg1.type != arg2.type) res = false;
  else if (arg1.type == TYPE_NULL) res = true;
  else if (arg1.type == TYPE_OBJECT) {
    res = arg1.obj == arg2.obj;
  } else if (arg1.type == TYPE_BOOL) {
    res = arg1.b == arg2.b;
  } else if (arg1.type == TYPE_INT) {
    res = arg1.i == arg2.i;
  } else if (arg1.type == TYPE_FLOAT) {
    res = arg1.f == arg2.f;
  } else assert(false);
  set_arg(state, instr->target, BOOL2VAL(res));
  state->instr = (Instr*)(instr + 1);
  return (FnWrap) { instr_fns[state->instr->type] };
}

static FnWrap vm_instr_instanceof(VMState *state) {
  InstanceofInstr *instr = (InstanceofInstr*) state->instr;
  
  Value proto_val = load_arg(state->frame, instr->proto);
  VM_ASSERT2(NOT_NULL(proto_val), "bad argument: instanceof null");
  bool res;
  if (!IS_OBJ(proto_val)) res = false; // nothing is instanceof 5
  else res = value_instance_of(state, load_arg(state->frame, instr->obj), AS_OBJ(proto_val));
  set_arg(state, instr->target, BOOL2VAL(res));
  
  state->instr = (Instr*)(instr + 1);
  return (FnWrap) { instr_fns[state->instr->type] };
}

static FnWrap vm_instr_set_constraint(VMState *state) __attribute__ ((hot));
static FnWrap vm_instr_set_constraint(VMState *state) {
  SetConstraintInstr *set_constraint_instr = (SetConstraintInstr*) state->instr;
  Value val = load_arg(state->frame, set_constraint_instr->obj);
  VM_ASSERT2(NOT_NULL(val), "can't set constraint on null");
  VM_ASSERT2(IS_OBJ(val), "can't set constraint on primitive");
  Object *obj = AS_OBJ(val);
  Value constraint_val = load_arg(state->frame, set_constraint_instr->constraint);
  VM_ASSERT2(IS_OBJ(constraint_val), "constraint must not be primitive!");
  Object *constraint = AS_OBJ(constraint_val);
  Object *string_base = state->shared->vcache.string_base;
  Value key_val = load_arg(state->frame, set_constraint_instr->key);
  StringObject *skey = (StringObject*) obj_instance_of(OBJ_OR_NULL(key_val), string_base);
  VM_ASSERT2(skey, "constraint key must be string");
  char *key = skey->value;
  
  char *error = object_set_constraint(state, obj, key, strlen(key), constraint);
  VM_ASSERT2(!error, "error while setting constraint: %s", error);
  
  state->instr = (Instr*)(set_constraint_instr + 1);
  return (FnWrap) { instr_fns[state->instr->type] };
}

static FnWrap vm_instr_assign_string_key(VMState *state) {
  AssignStringKeyInstr *aski = (AssignStringKeyInstr*) state->instr;
  Value obj_val = load_arg(state->frame, aski->obj);
  Value value = load_arg(state->frame, aski->value);
  char *key = aski->key;
  AssignType assign_type = aski->type;
  VM_ASSERT2(NOT_NULL(obj_val), "assignment to null");
  switch (assign_type) {
    case ASSIGN_PLAIN:
    {
      VM_ASSERT2(IS_OBJ(obj_val), "can't assign to primitive");
      char *error = object_set(state, AS_OBJ(obj_val), key, value);
      VM_ASSERT2(!error, error);
      break;
    }
    case ASSIGN_EXISTING:
    {
      Object *obj = closest_obj(state, obj_val);
      char *error = object_set_existing(state, obj, key, value);
      VM_ASSERT2(!error, error);
      break;
    }
    case ASSIGN_SHADOWING:
    {
      VM_ASSERT2(IS_OBJ(obj_val), "can't assign to primitive");
      bool key_set;
      char *error = object_set_shadowing(state, AS_OBJ(obj_val), key, value, &key_set);
      VM_ASSERT2(error == NULL, "while assigning '%s': %s", key, error);
      VM_ASSERT2(key_set, "key '%s' not found in object", key);
      break;
    }
  }
  state->instr = (Instr*)(aski + 1);
  return (FnWrap) { instr_fns[state->instr->type] };
}

static FnWrap vm_instr_set_constraint_string_key(VMState *state) {
  SetConstraintStringKeyInstr *scski = (SetConstraintStringKeyInstr*) state->instr;
  Value val = load_arg(state->frame, scski->obj);
  VM_ASSERT2(NOT_NULL(val), "can't set constraint on null");
  VM_ASSERT2(IS_OBJ(val), "can't set constraint on primitive");
  Object *obj = AS_OBJ(val);
  Value constraint_val = load_arg(state->frame, scski->constraint);
  VM_ASSERT2(IS_OBJ(constraint_val), "constraint must not be primitive!");
  Object *constraint = AS_OBJ(constraint_val);
  
  char *error = object_set_constraint(state, obj, scski->key_ptr, scski->key_len, constraint);
  VM_ASSERT2(!error, error);
  
  state->instr = (Instr*)(scski + 1);
  return (FnWrap) { instr_fns[state->instr->type] };
}

#include "vm/optimize.h"

static FnWrap vm_instr_call(VMState *state) __attribute__ ((hot));
static FnWrap vm_instr_call(VMState *state) {
  CallInstr *call_instr = (CallInstr*) state->instr;
  CallInfo *info = &call_instr->info;
  
  Instr *next_instr = (Instr*) ((char*) call_instr + call_instr->size);
  
  // inline call_internal/setup_call hotpath
  Value fn = load_arg(state->frame, info->fn);
  VM_ASSERT2(IS_OBJ(fn), "this is not a thing I can call.");
  Object *fn_obj_n = AS_OBJ(fn);
  
  if (fn_obj_n->parent == state->shared->vcache.function_base) {
    state->instr = next_instr;
    ((FunctionObject*)fn_obj_n)->fn_ptr(state, info);
    FnWrap next_instr[] = {
      (FnWrap) { vm_halt },
      (FnWrap) { instr_fns[state->instr->type] },
      (FnWrap) { vm_halt }
    };
    return next_instr[state->runstate];
  } else {
    // stackframe's instr_ptr will be pointed at the instr _after_ the call, but this messes up backtraces
    // solve explicitly
    state->frame->backtrace_belongs_to_p = &call_instr->base.belongs_to;
    state->frame->instr_ptr = next_instr;
    if (!setup_call(state, info)) return (FnWrap) { vm_halt };
    return (FnWrap) { instr_fns[state->instr->type] };
  }
}

static FnWrap vm_instr_call_function_direct(VMState *state) __attribute__ ((hot));
static FnWrap vm_instr_call_function_direct(VMState *state) {
  CallFunctionDirectInstr *instr = (CallFunctionDirectInstr*) state->instr;
  CallInfo *info = &instr->info;
  
  state->instr = (Instr*) ((char*) instr + instr->size);
  instr->fn(state, info);
  if (UNLIKELY(state->runstate != VM_RUNNING)) return (FnWrap) { vm_halt };
  
  return (FnWrap) { instr_fns[state->instr->type] };
}

static FnWrap vm_halt(VMState *state) {
  (void) state;
  return (FnWrap) { vm_halt };
}

static FnWrap vm_instr_return(VMState *state) {
  ReturnInstr *ret_instr = (ReturnInstr*) state->instr;
  Value res = load_arg(state->frame, ret_instr->ret);
  WriteArg target = state->frame->target;
  gc_remove_roots(state, &state->frame->frameroot_slots);
  vm_remove_frame(state);
  
  set_arg(state, target, res);
  
  if (UNLIKELY(!state->frame)) {
    return (FnWrap) { vm_halt };
  }
  
  state->instr = state->frame->instr_ptr;
  
  return (FnWrap) { instr_fns[state->instr->type] };
}

static FnWrap vm_instr_br(VMState *state) {
  BranchInstr *br_instr = (BranchInstr*) state->instr;
  int blk = br_instr->blk;
  VM_ASSERT2_SLOT(blk < state->frame->uf->body.blocks_len, "slot numbering error");
  state->instr = (Instr*) ((char*) state->frame->uf->body.instrs_ptr + state->frame->uf->body.blocks_ptr[blk].offset);
  state->frame->prev_block = state->frame->block;
  state->frame->block = blk;
  return (FnWrap) { instr_fns[state->instr->type] };
}

static FnWrap vm_instr_testbr(VMState *state) __attribute__ ((hot));
static FnWrap vm_instr_testbr(VMState *state) {
  TestBranchInstr *tbr_instr = (TestBranchInstr*) state->instr;
  int true_blk = tbr_instr->true_blk, false_blk = tbr_instr->false_blk;
  Value test_value = load_arg(state->frame, tbr_instr->test);
  
  bool test = value_is_truthy(test_value);
  
  int target_blk = test ? true_blk : false_blk;
  state->instr = (Instr*) ((char*) state->frame->uf->body.instrs_ptr + state->frame->uf->body.blocks_ptr[target_blk].offset);
  state->frame->prev_block = state->frame->block;
  state->frame->block = target_blk;
  return (FnWrap) { instr_fns[state->instr->type] };
}

static FnWrap vm_instr_phi(VMState *state) {
  PhiInstr *phi = (PhiInstr*) state->instr;
  if (state->frame->prev_block == phi->block1) {
    set_arg(state, phi->target, load_arg(state->frame, phi->arg1));
  } else if (state->frame->prev_block == phi->block2) {
    set_arg(state, phi->target, load_arg(state->frame, phi->arg2));
  } else VM_ASSERT2(false, "phi block error: arrived here from block not in list: [%i, %i], but came from %i",
                    phi->block1, phi->block2, state->frame->prev_block);
  
  state->instr = (Instr*)(phi + 1);
  return (FnWrap) { instr_fns[state->instr->type] };
}

static FnWrap vm_instr_define_refslot(VMState *state) {
  DefineRefslotInstr *dri = (DefineRefslotInstr*) state->instr;
  
  int target_refslot = dri->target_refslot;
  int obj_slot = dri->obj_slot;
  VM_ASSERT2_SLOT(obj_slot < state->frame->slots_len, "slot numbering error");
  
  Object *obj = closest_obj(state, state->frame->slots_ptr[obj_slot]);
  VM_ASSERT2(obj, "cannot define refslot for null obj");
  
  TableEntry *entry = object_lookup_ref_with_hash(obj, dri->key_ptr, dri->key_len, dri->key_hash);
  VM_ASSERT2(entry, "key not in object");
  state->frame->refslots_ptr[target_refslot] = entry;
  
  state->instr = (Instr*)(dri + 1);
  return (FnWrap) { instr_fns[state->instr->type] };
}

static FnWrap vm_instr_move(VMState *state) {
  MoveInstr *mi = (MoveInstr*) state->instr;
  
  set_arg(state, mi->target, load_arg(state->frame, mi->source));
  
  state->instr = (Instr*)(mi + 1);
  return (FnWrap) { instr_fns[state->instr->type] };
}

static FnWrap vm_instr_alloc_static_object(VMState *state) __attribute__ ((hot));
static FnWrap vm_instr_alloc_static_object(VMState *state) {
  AllocStaticObjectInstr *asoi = (AllocStaticObjectInstr*) state->instr;
  
  int target_slot = asoi->target_slot, parent_slot = asoi->parent_slot;
  VM_ASSERT2_SLOT(target_slot < state->frame->slots_len, "slot numbering error");
  VM_ASSERT2_SLOT(parent_slot < state->frame->slots_len, "slot numbering error");
  Object *parent_obj = OBJ_OR_NULL(state->frame->slots_ptr[parent_slot]);
  VM_ASSERT2(!parent_obj || !(parent_obj->flags & OBJ_NOINHERIT), "cannot inherit from object marked no-inherit");
  Object *obj = AS_OBJ(make_object(state, parent_obj));
  
  // TODO table_clone
  obj->tbl = ((Object*)(asoi+1))->tbl;
  int tbl_len = sizeof(TableEntry) * obj->tbl.entries_num;
  obj->tbl.entries_ptr = cache_alloc(tbl_len);
  memcpy(obj->tbl.entries_ptr, ((Object*)(asoi+1))->tbl.entries_ptr, tbl_len);
  
  for (int i = 0; i < asoi->info_len; ++i) {
    StaticFieldInfo *info = &ASOI_INFO(asoi)[i];
    VM_ASSERT2_SLOT(info->slot < state->frame->slots_len, "slot numbering error");
    TableEntry *entry = &obj->tbl.entries_ptr[info->tbl_offset];
    entry->value = state->frame->slots_ptr[info->slot]; // 0 is null
    if (info->constraint) {
      entry->constraint = info->constraint;
      VM_ASSERT2(value_instance_of(state, entry->value, info->constraint), "type constraint violated on variable");
    }
    state->frame->refslots_ptr[info->refslot] = entry;
  }
  
  obj->flags = OBJ_CLOSED;
  
  state->frame->slots_ptr[target_slot] = OBJ2VAL(obj);
  
  state->instr = (Instr*)((char*) asoi
                          + sizeof(AllocStaticObjectInstr)
                          + sizeof(Object)
                          + sizeof(StaticFieldInfo) * asoi->info_len);
  return (FnWrap) { instr_fns[state->instr->type] };
}

void vm_update_frame(VMState *state) {
  state->frame->instr_ptr = state->instr;
}

static void vm_step(VMState *state) {
  state->instr = state->frame->instr_ptr;
  VMInstrFn fn = (VMInstrFn) instr_fns[state->instr->type];
  // fuzz to prevent profiler aliasing
  int limit = 128 + (state->shared->cyclecount % 64);
  int i;
  for (i = 0; i < limit && fn != vm_halt; i++) {
    /*{
      fprintf(stderr, "run [%p] <%i> {%p, %i, %i} ", (void*) state->instr, state->frame->block, (void*) state->frame, state->frame->slots_len, state->frame->refslots_len);
      Instr *instr = state->instr;
      dump_instr(state, &instr);
    }*/
    fn = fn(state).self;
    fn = fn(state).self;
    fn = fn(state).self;
    fn = fn(state).self;
    fn = fn(state).self;
    fn = fn(state).self;
    fn = fn(state).self;
    fn = fn(state).self;
    fn = fn(state).self;
  }
  state->shared->cyclecount += i * 9;
  if (state->frame) vm_update_frame(state);
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
  instr_fns[INSTR_IDENTICAL] = vm_instr_identical;
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
  instr_fns[INSTR_DEFINE_REFSLOT] = vm_instr_define_refslot;
  instr_fns[INSTR_MOVE] = vm_instr_move;
  instr_fns[INSTR_CALL_FUNCTION_DIRECT] = vm_instr_call_function_direct;
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
    if (state->runstate == VM_ERRORED) return;
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
