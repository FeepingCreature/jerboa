#include "vm/vm.h"

#include <stdarg.h>
#include <stdio.h>

#include "vm/call.h"
#include "vm/dump.h"
#include "gc.h"

const long long sample_stepsize = 200000LL; // 0.2ms

static void *vm_stack_alloc_internal(VMState * __restrict__ state, int size) {
  VMSharedState * __restrict__ shared = state->shared;
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
  if (res) bzero(res, size);
  return res;
}

void *vm_stack_alloc_uninitialized(VMState *state, int size) {
  return vm_stack_alloc_internal(state, size);
}

void vm_stack_free(VMState *state, void *ptr, int size) {
  VMSharedState *shared = state->shared;
  if (size == -1) size = (char*) shared->stack_data_ptr + shared->stack_data_offset - (char*) ptr; // Only use this when there is no other way!
  int new_offset = shared->stack_data_offset - size;
  (void) ptr; assert(ptr == (char*) shared->stack_data_ptr + new_offset); // must free in reverse order!
  shared->stack_data_offset = new_offset;
}

void vm_alloc_frame(VMState *state, int slots, int refslots) {
  Callframe * __restrict__ cf = (Callframe*) vm_stack_alloc_uninitialized(state, sizeof(Callframe) + sizeof(TableEntry*) * refslots + sizeof(Value) * slots);
  if (!cf) return; // stack overflow
  cf->uf = NULL;
  cf->above = state->frame;
  cf->last_stack_obj = NULL;
  cf->block = 0;
  
  Value *slots_ptr = (Value*) ((unsigned char*) cf + sizeof(Callframe) + sizeof(TableEntry*) * refslots);
  // no need to zero refslots, as they're not gc'd
  bzero(slots_ptr, sizeof(Value) * slots);
  
  state->frame = cf;
  gc_add_roots(state, slots_ptr, slots, &cf->frameroot_slots);
}

void vm_error(VMState *state, const char *fmt, ...) {
  assert(state->runstate != VM_ERRORED);
  char *errorstr;
  va_list ap;
  va_start(ap, fmt);
  errorstr = my_vasprintf(fmt, ap);
  va_end(ap);
  state->runstate = VM_ERRORED;
  state->error = errorstr;
  state->backtrace_depth = 0;
}

void vm_remove_frame(VMState *state) {
  Callframe *cf = state->frame;
  Object *obj = cf->last_stack_obj;
  while (obj) {
    obj_free_aux(obj);
    Object *prev_obj = obj->prev;
    vm_stack_free(state, obj, obj->size);
    obj = prev_obj;
  }
  state->frame = cf->above;
  if (LIKELY(cf->uf)) {
    vm_stack_free(state, cf, sizeof(Callframe) + sizeof(TableEntry*) * cf->uf->refslots + sizeof(Value) * cf->uf->slots);
  } else {
    vm_stack_free(state, cf, -1);
  }
}

static FnWrap vm_instr_return(VMState *state) FAST_FN;
const static __thread struct __attribute__((__packed__)) {
  CallInstr stub_call;
  ReturnInstr stub_ret0;
} stub_instrs = {
  .stub_call = { { .type = INSTR_CALL }, .size = sizeof(CallInstr) },
#ifndef NDEBUG
  .stub_ret0 = { { .type = INSTR_RETURN, .fn = vm_instr_return }, .ret = { .kind = ARG_SLOT, .slot = { .offset = sizeof(Callframe), .is_resolved = true } } }
#else
  .stub_ret0 = { { .type = INSTR_RETURN, .fn = vm_instr_return }, .ret = { .kind = ARG_SLOT, .slot = { .offset = sizeof(Callframe) } } }
#endif
};

void setup_stub_frame(VMState *state, int slots) {
  if (state->frame) {
    state->frame->instr_ptr = state->instr;
    state->frame->return_next_instr = (Instr*) ((char*) state->instr + instr_size(state->instr));
  }
  vm_alloc_frame(state, slots + 1, 0);
  state->instr = (Instr*) &stub_instrs;
  state->frame->return_next_instr = (Instr*) &stub_instrs.stub_ret0;
}

void vm_print_backtrace(VMState *state) {
  int k = state->backtrace_depth;
  if (state->backtrace) fprintf(stderr, "%s", state->backtrace);
  while (state) {
    if (state->frame) state->frame->instr_ptr = state->instr;
    for (Callframe *curf = state->frame; curf; k++, curf = curf->above) {
      Instr *instr = curf->instr_ptr;
      if (!curf->uf) continue; // stub frame
      FileRange *belongs_to = *instr_belongs_to_p(&curf->uf->body, instr);
      assert(belongs_to);
      
      const char *file, *fn;
      TextRange line1, line2;
      int row1, row2, col1, col2;
      bool found = find_text_pos(belongs_to->text_from, &file, &fn, &line1, &row1, &col1);
      (void) found;
      assert(found);
      found = find_text_pos(belongs_to->text_from + belongs_to->text_len, &file, &fn, &line2, &row2, &col2);
      assert(found);
      int len1 = (int) (line1.end - line1.start - 1);
      if (col1 > len1) col1 = len1;
      int len2 = (int) (line2.end - line2.start - 1);
      if (col2 > len2) col2 = len2;
      // if (strcmp(file, "test4.jb") == 0 && row == 18) __asm__("int $3");
      fprintf(stderr, "#%i\t%s:%i\t", k+1, file, row1+1); // file:line
      fprintf(stderr, "%.*s", col1, line1.start); // line up to the range start
      format_bold(stderr);
      fprintf(stderr, "%.*s", belongs_to->text_len, belongs_to->text_from); // actual range
      format_reset(stderr);
      fprintf(stderr, "%.*s\n", len2 - col2, line2.start + col2); // end of range to end of line
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
    FileRange *belongs_to = *instr_belongs_to_p(&curf->uf->body, instr);
    
    const char *file, *fn;
    TextRange line;
    int row, col;
    bool found = find_text_pos(belongs_to->text_from, &file, &fn, &line, &row, &col);
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
  // TODO record actual cycles instead of samples
  HashTable *excl_table = &state->shared->profstate.excl_table;
  HashTable *incl_table = &state->shared->profstate.incl_table;
  
  VMState *curstate = state;
  // fprintf(stderr, "generate backtrace\n");
  // fprintf(stderr, "------\n");
  // vm_print_backtrace(state);
  int k = 0;
  Callframe *prev_frame = NULL;
  while (curstate) {
    if (curstate->frame) curstate->frame->instr_ptr = curstate->instr;
    for (Callframe *curf = curstate->frame; curf; k++, curf = curf->above) {
      Instr *instr = curf->instr_ptr;
      if (!curf->uf) continue; // stub frame
      
      FileRange *belongs_to = *instr_belongs_to_p(&curf->uf->body, instr);
      assert(belongs_to);
      
      // ranges are unique (and instrs must live as long as the vm state lives anyways)
      // so we can just use the pointer stored in the instr as the key
      FastKey key = fixed_pointer_key(belongs_to);
      
      if (!prev_frame) { // top frame
        TableEntry *freeptr;
        TableEntry *entry_p = table_lookup_alloc_prepared(excl_table, &key, &freeptr);
        if (entry_p) entry_p->value.i ++;
        else freeptr->value = INT2VAL(1);
      } else {
        TableEntry *freeptr;
        TableEntry *entry_p = table_lookup_alloc_prepared(incl_table, &key, &freeptr);
        if (freeptr) {
          freeptr->value.obj = calloc(sizeof(HashTable), 1);
          entry_p = freeptr;
        }
        HashTable *sub_table = (HashTable*) entry_p->value.obj;
        // assert(prev_frame->uf->body.function_range);
        FastKey next_fn_key = fixed_pointer_key(prev_frame->uf->body.function_range);
        TableEntry *subentry_p = table_lookup_alloc_prepared(sub_table, &next_fn_key, &freeptr);
        if (subentry_p) subentry_p->value.i ++;
        else freeptr->value = INT2VAL(1);
      }
      prev_frame = curf;
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
      fprintf(stderr, "warning: collecting profiling info took %i%% of the last step.\n", (int) (ns_taken * 100LL / sample_stepsize));
      num_msg_printed++;
      if (num_msg_printed > 10) {
        fprintf(stderr, "warning will not be shown again.\n");
      }
    }
  }
}

static VMInstrFn instr_fns[INSTR_LAST] = {0};

static FnWrap vm_instr_alloc_object(VMState *state) FAST_FN;
static FnWrap vm_instr_alloc_object(VMState *state) {
  AllocObjectInstr * __restrict__ alloc_obj_instr = (AllocObjectInstr*) state->instr;
  Slot target_slot = alloc_obj_instr->target_slot, parent_slot = alloc_obj_instr->parent_slot;
  Object *parent_obj = OBJ_OR_NULL(read_slot(state->frame, parent_slot));
  VM_ASSERT2(!parent_obj || !(parent_obj->flags & OBJ_NOINHERIT), "cannot inherit from object marked no-inherit");
  write_slot(state->frame, target_slot, make_object(state, parent_obj, alloc_obj_instr->alloc_stack));
  state->instr = (Instr*)(alloc_obj_instr + 1);
  STEP_VM;
}

static FnWrap vm_instr_alloc_int_object(VMState *state) FAST_FN;
static FnWrap vm_instr_alloc_int_object(VMState *state) {
  AllocIntObjectInstr * __restrict__ alloc_int_obj_instr = (AllocIntObjectInstr*) state->instr;
  int value = alloc_int_obj_instr->value;
  set_arg(state, alloc_int_obj_instr->target, INT2VAL(value));
  state->instr = (Instr*)(alloc_int_obj_instr + 1);
  STEP_VM;
}

static FnWrap vm_instr_alloc_bool_object(VMState *state) FAST_FN;
static FnWrap vm_instr_alloc_bool_object(VMState *state) {
  AllocBoolObjectInstr * __restrict__ alloc_bool_obj_instr = (AllocBoolObjectInstr*) state->instr;
  bool value = alloc_bool_obj_instr->value;
  set_arg(state, alloc_bool_obj_instr->target, BOOL2VAL(value));
  state->instr = (Instr*)(alloc_bool_obj_instr + 1);
  STEP_VM;
}

static FnWrap vm_instr_alloc_float_object(VMState *state) FAST_FN;
static FnWrap vm_instr_alloc_float_object(VMState *state) {
  AllocFloatObjectInstr * __restrict__ alloc_float_obj_instr = (AllocFloatObjectInstr*) state->instr;
  float value = alloc_float_obj_instr->value;
  set_arg(state, alloc_float_obj_instr->target, FLOAT2VAL(value));
  state->instr = (Instr*)(alloc_float_obj_instr + 1);
  STEP_VM;
}

static FnWrap vm_instr_alloc_array_object(VMState *state) FAST_FN;
static FnWrap vm_instr_alloc_array_object(VMState *state) {
  AllocArrayObjectInstr * __restrict__ alloc_array_obj_instr = (AllocArrayObjectInstr*) state->instr;
  set_arg(state, alloc_array_obj_instr->target, make_array(state, NULL, 0, true));
  state->instr = (Instr*)(alloc_array_obj_instr + 1);
  STEP_VM;
}

static FnWrap vm_instr_alloc_string_object(VMState *state) FAST_FN;
static FnWrap vm_instr_alloc_string_object(VMState *state) {
  AllocStringObjectInstr * __restrict__ alloc_string_obj_instr = (AllocStringObjectInstr*) state->instr;
  char *value = alloc_string_obj_instr->value;
  set_arg(state, alloc_string_obj_instr->target, make_string_static(state, value));
  state->instr = (Instr*)(alloc_string_obj_instr + 1);
  STEP_VM;
}

static FnWrap vm_instr_alloc_closure_object(VMState *state) FAST_FN;
static FnWrap vm_instr_alloc_closure_object(VMState *state) {
  AllocClosureObjectInstr * __restrict__ alloc_closure_obj_instr = (AllocClosureObjectInstr*) state->instr;
  Slot context_slot = alloc_closure_obj_instr->base.context_slot;
  Value context = read_slot(state->frame, context_slot);
  VM_ASSERT2(IS_OBJ(context), "bad slot type");
  Object *context_obj = AS_OBJ(context);
  set_arg(state, alloc_closure_obj_instr->target, make_closure_fn(state, context_obj, alloc_closure_obj_instr->fn));
  state->instr = (Instr*)(alloc_closure_obj_instr + 1);
  STEP_VM;
}

static FnWrap vm_instr_free_object(VMState *state) FAST_FN;
static FnWrap vm_instr_free_object(VMState *state) {
  FreeObjectInstr * __restrict__ free_object_instr = (FreeObjectInstr*) state->instr;
  Slot slot = free_object_instr->obj_slot;
  Value val = read_slot(state->frame, slot);
  // non-object values are always OBJ_CLOSED
  if (LIKELY(free_object_instr->on_stack)) {
    // this instr should only be applied to static objects
    VM_ASSERT2_DEBUG(IS_OBJ(val), "tried to free object that wasn't stack allocated");
    AS_OBJ(val)->flags |= OBJ_STACK_FREED;
    
    Callframe * __restrict__ cf = state->frame;
#ifndef NDEBUG
    for (int i = 0; i < SLOTS_LEN(cf); i++) {
      Slot slot_i = (Slot) { .index = i };
      resolve_slot_ref(cf->uf, &slot_i);
      if (slot_i.offset != slot.offset && read_slot(cf, slot_i).type == TYPE_OBJECT && read_slot(cf, slot_i).obj == AS_OBJ(val)) {
        fprintf(stderr, "bad - stack reference to freed object");
        abort();
      }
    }
#endif
    // important! don't gc scan anymore
    write_slot(cf, slot, VNULL);
    
    Object *last_stack_obj = cf->last_stack_obj;
    while (last_stack_obj && last_stack_obj->flags & OBJ_STACK_FREED) {
      obj_free_aux(last_stack_obj); // hence not necessary
      Object * __restrict__ prev_obj = last_stack_obj->prev;
      vm_stack_free(state, last_stack_obj, last_stack_obj->size);
      last_stack_obj = prev_obj;
    }
    cf->last_stack_obj = last_stack_obj;
  } else abort();
  state->instr = (Instr*)(free_object_instr + 1);
  STEP_VM;
}

static FnWrap vm_instr_close_object(VMState *state) FAST_FN;
static FnWrap vm_instr_close_object(VMState *state) {
  CloseObjectInstr * __restrict__ close_object_instr = (CloseObjectInstr*) state->instr;
  Slot slot = close_object_instr->slot;
  Value val = read_slot(state->frame, slot);
  // non-object values are always OBJ_CLOSED
  VM_ASSERT2(IS_OBJ(val) && !(AS_OBJ(val)->flags & OBJ_CLOSED), "object is already closed!");
  AS_OBJ(val)->flags |= OBJ_CLOSED;
  state->instr = (Instr*)(close_object_instr + 1);
  STEP_VM;
}

static FnWrap vm_instr_freeze_object(VMState *state) FAST_FN;
static FnWrap vm_instr_freeze_object(VMState *state) {
  FreezeObjectInstr * __restrict__ freeze_object_instr = (FreezeObjectInstr*) state->instr;
  Slot slot = freeze_object_instr->slot;
  Value val = read_slot(state->frame, slot);
  // non-object values are always OBJ_FROZEN
  VM_ASSERT2(IS_OBJ(val) && !(AS_OBJ(val)->flags & OBJ_FROZEN), "object is already frozen!");
  AS_OBJ(val)->flags |= OBJ_FROZEN;
  state->instr = (Instr*)(freeze_object_instr + 1);
  STEP_VM;
}

FnWrap call_internal(VMState *state, CallInfo *info, Instr *instr_after_call) {
  if (!setup_call(state, info, instr_after_call)) {
    return (FnWrap) { vm_halt };
  }
  // STEP_VM;
  return (FnWrap) { state->instr->fn }; // not safe to recurse here!
}

static FnWrap vm_instr_access(VMState *state) FAST_FN;
static FnWrap vm_instr_access(VMState *state) {
  AccessInstr * __restrict__ access_instr = (AccessInstr*) state->instr;
  
  Value val = load_arg(state->frame, access_instr->obj);
  Object *obj = closest_obj(state, val);
  
  char *key;
  
  Value key_val = load_arg(state->frame, access_instr->key);
  VM_ASSERT2(NOT_NULL(key_val), "key is null");
  Object *string_base = state->shared->vcache.string_base;
  Object *key_obj = OBJ_OR_NULL(key_val);
  StringObject *skey = (StringObject*) obj_instance_of(key_obj, string_base);
  bool object_found = false;
  if (skey) {
    key = skey->value;
    // otherwise, skey->value is independent of skey
    // TODO length in StringObject
    FastKey fkey = prepare_key(key, strlen(key));
    set_arg(state, access_instr->target, object_lookup_p(obj, &fkey, &object_found));
  }
  if (!object_found) {
    Value index_op = OBJECT_LOOKUP(obj, __slice);
    if (NOT_NULL(index_op)) {
      CallInfo *info = alloca(sizeof(CallInfo) + sizeof(Arg));
      info->args_len = 1;
      info->this_arg = (Arg) { .kind = ARG_VALUE, .value = val };
      info->fn = (Arg) { .kind = ARG_VALUE, .value = index_op };
      info->target = access_instr->target;
      INFO_ARGS_PTR(info)[0] = access_instr->key;
      
      return call_internal(state, info, (Instr*)(access_instr + 1));
    }
  }
  if (!object_found) {
    if (skey) {
      VM_ASSERT2(false, "[1] property not found: '%s'", key);
    } else {
      VM_ASSERT2(false, "[2] property not found!");
    }
  }
  state->instr = (Instr*)(access_instr + 1);
  STEP_VM;
}

#include "print.h"
static FnWrap vm_instr_access_string_key_index_fallback(VMState *state, AccessStringKeyInstr *aski, Instr *instr_after) {
  Value val = load_arg(state->frame, aski->obj);
  Object *obj = closest_obj(state, val);
  Value index_op = OBJECT_LOOKUP(obj, __slice);
  if (NOT_NULL(index_op)) {
    Value key = make_string(state, aski->key.key, strlen(aski->key.key));
    write_slot(state->frame, aski->key_slot, key);
    
    CallInfo *info = alloca(sizeof(CallInfo) + sizeof(Arg));
    info->args_len = 1;
    info->this_arg = (Arg) { .kind = ARG_VALUE, .value = val };
    info->fn = (Arg) { .kind = ARG_VALUE, .value = index_op };
    info->target = aski->target;
    INFO_ARGS_PTR(info)[0] = (Arg) { .kind = ARG_SLOT, .slot = aski->key_slot };
    
    return call_internal(state, info, instr_after);
  } else {
    // Value val = VNULL;
    // if (obj) val = OBJ2VAL(obj);
    // print_recursive(state, stderr, val, true);
    fprintf(stderr, "\n");
    VM_ASSERT(false, "[3] property not found: '%s'", aski->key.key) (FnWrap) { vm_halt };
  }
}

static FnWrap vm_instr_access_string_key(VMState *state) FAST_FN;
static FnWrap vm_instr_access_string_key(VMState *state) {
  AccessStringKeyInstr * __restrict__ aski = (AccessStringKeyInstr*) state->instr;
  
  Object *obj = closest_obj(state, load_arg(state->frame, aski->obj));
  
  bool object_found = false;
  Value result = object_lookup_p(obj, &aski->key, &object_found);
  
  if (UNLIKELY(!object_found)) {
    return vm_instr_access_string_key_index_fallback(state, aski, (Instr*)(aski + 1));
  } else {
    set_arg(state, aski->target, result);
    state->instr = (Instr*)(aski + 1);
    STEP_VM;
  }
}

static FnWrap vm_instr_assign(VMState *state) FAST_FN;
static FnWrap vm_instr_assign(VMState *state) {
  AssignInstr * __restrict__ assign_instr = (AssignInstr*) state->instr;
  Slot target_slot = assign_instr->target_slot;
  VM_ASSERT2(NOT_NULL(load_arg(state->frame, assign_instr->key)), "key slot null"); // TODO see above
  Value obj_val = load_arg(state->frame, assign_instr->obj);
  Object *obj = closest_obj(state, obj_val);
  Value value_val = load_arg(state->frame, assign_instr->value);
  Object *string_base = state->shared->vcache.string_base;
  Value key_val = load_arg(state->frame, assign_instr->key);
  StringObject *skey = (StringObject*) obj_instance_of(OBJ_OR_NULL(key_val), string_base);
  if (!skey) {
    // non-string key, goes to []=
    Value index_assign_op = OBJECT_LOOKUP(obj, __slice_assign);
    if (NOT_NULL(index_assign_op)) {
      CallInfo *info = alloca(sizeof(CallInfo) + sizeof(Arg) * 2);
      info->args_len = 2;
      info->this_arg = (Arg) { .kind = ARG_VALUE, .value = obj_val };
      info->fn = (Arg) { .kind = ARG_VALUE, .value = index_assign_op };
      info->target = (WriteArg) { .kind = ARG_SLOT, .slot = target_slot };
      INFO_ARGS_PTR(info)[0] = assign_instr->key;
      INFO_ARGS_PTR(info)[1] = assign_instr->value;
      
      return call_internal(state, info, (Instr*)(assign_instr + 1));
    }
    VM_ASSERT2(false, "key is not string and no '[]=' is set");
  }
  char *key_ptr = skey->value;
  FastKey key = prepare_key(key_ptr, strlen(key_ptr));
  AssignType assign_type = assign_instr->type;
  // fprintf(stderr, "> obj set %p . '%s' = %p\n", (void*) obj, key, (void*) value_obj);
  VM_ASSERT2(obj, "assignment to null object");
  switch (assign_type) {
    case ASSIGN_PLAIN:
    {
      char *error = object_set(state, obj, &key, value_val);
      VM_ASSERT2(!error, error);
      break;
    }
    case ASSIGN_EXISTING:
    {
      char *error = object_set_existing(state, obj, &key, value_val);
      VM_ASSERT2(!error, error);
      break;
    }
    case ASSIGN_SHADOWING:
    {
      bool key_set;
      char *error = object_set_shadowing(state, obj, &key, value_val, &key_set);
      VM_ASSERT2(error == NULL, "while shadow-assigning: %s", error);
      VM_ASSERT2(key_set, "key '%s' not found in object", key);
      break;
    }
  }
  state->instr = (Instr*)(assign_instr + 1);
  STEP_VM;
}

static FnWrap vm_instr_key_in_obj(VMState *state) FAST_FN;
static FnWrap vm_instr_key_in_obj(VMState *state) {
  KeyInObjInstr * __restrict__ key_in_obj_instr = (KeyInObjInstr*) state->instr;
  Value val = load_arg(state->frame, key_in_obj_instr->obj);
  Object *obj = closest_obj(state, val);
  Object *string_base = state->shared->vcache.string_base;
  Value key_val = load_arg(state->frame, key_in_obj_instr->key);
  VM_ASSERT2(NOT_NULL(key_val), "key is null");
  StringObject *skey = (StringObject*) obj_instance_of(OBJ_OR_NULL(key_val), string_base);
  bool object_found = false;
  char *key;
  if (skey) {
    key = skey->value;
    /*if (key_in_obj_instr->key.kind == ARG_VALUE) {
      printf(": %s\n", key);
    }*/
    FastKey fkey = prepare_key(key, strlen(key));
    object_lookup_p(obj, &fkey, &object_found);
    set_arg(state, key_in_obj_instr->target, BOOL2VAL(object_found));
  }
  
  if (!object_found) {
    Value in_overload_op = OBJECT_LOOKUP(obj, in);
    if (NOT_NULL(in_overload_op)) {
      CallInfo *info = alloca(sizeof(CallInfo) + sizeof(Arg) * 1);
      info->args_len = 1;
      info->this_arg = (Arg) { .kind = ARG_VALUE, .value = val };
      info->fn = (Arg) { .kind = ARG_VALUE, .value = in_overload_op };
      info->target = key_in_obj_instr->target;
      INFO_ARGS_PTR(info)[0] = key_in_obj_instr->key;
      
      return call_internal(state, info, (Instr*)(key_in_obj_instr + 1));
    }
  }
  
  state->instr = (Instr*)(key_in_obj_instr + 1);
  STEP_VM;
}

static FnWrap vm_instr_string_key_in_obj(VMState *state) FAST_FN;
static FnWrap vm_instr_string_key_in_obj(VMState *state) {
  StringKeyInObjInstr * __restrict__ skioi = (StringKeyInObjInstr*) state->instr;
  Object *obj = closest_obj(state, load_arg(state->frame, skioi->obj));
  // printf(": %.*s\n", (int) skioi->key.len, skioi->key.ptr);
  bool object_found = false;
  object_lookup_p(obj, &skioi->key, &object_found);
  set_arg(state, skioi->target, BOOL2VAL(object_found));
  
  state->instr = (Instr*)(skioi + 1);
  STEP_VM;
}

static FnWrap vm_instr_identical(VMState *state) FAST_FN;
static FnWrap vm_instr_identical(VMState *state) {
  IdenticalInstr * __restrict__ instr= (IdenticalInstr*) state->instr;
  Value arg1 = load_arg(state->frame, instr->obj1);
  Value arg2 = load_arg(state->frame, instr->obj2);
  bool res = values_identical(arg1, arg2);
  set_arg(state, instr->target, BOOL2VAL(res));
  state->instr = (Instr*)(instr + 1);
  STEP_VM;
}

static FnWrap vm_instr_instanceof(VMState *state) FAST_FN;
static FnWrap vm_instr_instanceof(VMState *state) {
  InstanceofInstr * __restrict__ instr = (InstanceofInstr*) state->instr;
  
  Value proto_val = load_arg(state->frame, instr->proto);
  VM_ASSERT2(NOT_NULL(proto_val), "bad argument: instanceof null");
  bool res;
  if (!IS_OBJ(proto_val)) res = false; // nothing is instanceof 5
  else res = value_instance_of(state, load_arg(state->frame, instr->obj), AS_OBJ(proto_val));
  set_arg(state, instr->target, BOOL2VAL(res));
  
  state->instr = (Instr*)(instr + 1);
  STEP_VM;
}

static FnWrap vm_instr_set_constraint(VMState *state) FAST_FN;
static FnWrap vm_instr_set_constraint(VMState *state) {
  SetConstraintInstr * __restrict__ set_constraint_instr = (SetConstraintInstr*) state->instr;
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
  
  FastKey fkey = prepare_key(key, strlen(key));
  char *error = object_set_constraint(state, obj, &fkey, constraint);
  VM_ASSERT2(!error, "error while setting constraint: %s", error);
  
  state->instr = (Instr*)(set_constraint_instr + 1);
  STEP_VM;
}

static FnWrap vm_instr_assign_string_key(VMState *state) FAST_FN;
static FnWrap vm_instr_assign_string_key(VMState *state) {
  AssignStringKeyInstr * __restrict__ aski = (AssignStringKeyInstr*) state->instr;
  Value obj_val = load_arg(state->frame, aski->obj);
  Value value = load_arg(state->frame, aski->value);
  AssignType assign_type = aski->type;
  VM_ASSERT2(NOT_NULL(obj_val), "assignment to null");
  switch (assign_type) {
    case ASSIGN_PLAIN:
    {
      VM_ASSERT2(IS_OBJ(obj_val), "can't assign to primitive");
      char *error = object_set(state, AS_OBJ(obj_val), &aski->key, value);
      VM_ASSERT2(!error, error);
      break;
    }
    case ASSIGN_EXISTING:
    {
      Object *obj = closest_obj(state, obj_val);
      char *error = object_set_existing(state, obj, &aski->key, value);
      VM_ASSERT2(!error, error);
      break;
    }
    case ASSIGN_SHADOWING:
    {
      VM_ASSERT2(IS_OBJ(obj_val), "can't assign to primitive");
      bool key_set;
      char *error = object_set_shadowing(state, AS_OBJ(obj_val), &aski->key, value, &key_set);
      VM_ASSERT2(error == NULL, "while shadow-assigning '%s': %s", aski->key.key, error);
      if (!key_set) { // fall back to index?
        Value index_assign_op = OBJECT_LOOKUP(AS_OBJ(obj_val), __slice_assign);
        if (NOT_NULL(index_assign_op)) {
          Value key = make_string(state, aski->key.key, strlen(aski->key.key));
          CallInfo *info = alloca(sizeof(CallInfo) + sizeof(Arg) * 2);
          info->args_len = 2;
          info->this_arg = (Arg) { .kind = ARG_VALUE, .value = obj_val };
          info->fn = (Arg) { .kind = ARG_VALUE, .value = index_assign_op };
          info->target = (WriteArg) { .kind = ARG_SLOT, .slot = aski->target_slot };
          INFO_ARGS_PTR(info)[0] = (Arg) { .kind = ARG_VALUE, .value = key };
          INFO_ARGS_PTR(info)[1] = (Arg) { .kind = ARG_VALUE, .value = value };
          
          return call_internal(state, info, (Instr*)(aski + 1));
        }
      }
      VM_ASSERT2(key_set, "key '%s' not found in object", aski->key.key);
      break;
    }
  }
  state->instr = (Instr*)(aski + 1);
  STEP_VM;
}

static FnWrap vm_instr_set_constraint_string_key(VMState *state) FAST_FN;
static FnWrap vm_instr_set_constraint_string_key(VMState *state) {
  SetConstraintStringKeyInstr * __restrict__ scski = (SetConstraintStringKeyInstr*) state->instr;
  Value val = load_arg(state->frame, scski->obj);
  VM_ASSERT2(NOT_NULL(val), "can't set constraint on null");
  VM_ASSERT2(IS_OBJ(val), "can't set constraint on primitive");
  Object *obj = AS_OBJ(val);
  Value constraint_val = load_arg(state->frame, scski->constraint);
  VM_ASSERT2(IS_OBJ(constraint_val), "constraint must not be primitive!");
  Object *constraint = AS_OBJ(constraint_val);
  
  char *error = object_set_constraint(state, obj, &scski->key, constraint);
  VM_ASSERT2(!error, error);
  
  state->instr = (Instr*)(scski + 1);
  STEP_VM;
}

#include "vm/optimize.h"

static FnWrap vm_instr_call(VMState *state) FAST_FN;
static FnWrap vm_instr_call(VMState *state) {
  CallInstr * __restrict__ call_instr = (CallInstr*) state->instr;
  CallInfo *info = &call_instr->info;
  
  return call_internal(state, info, (Instr*) ((char*) call_instr + call_instr->size));
}

static FnWrap vm_instr_call_function_direct(VMState *state) FAST_FN;
static FnWrap vm_instr_call_function_direct(VMState *state) {
  CallFunctionDirectInstr * __restrict__ instr = (CallFunctionDirectInstr*) state->instr;
  CallInfo *info = &instr->info;
  
  // cache beforehand, in case the function (wrongly) wants to set up its own stub call like apply
  Instr *prev_instr = state->instr;
  
  Instr *next_instr = (Instr*) ((char*) instr + instr->size);
  instr->fn(state, info);
  if (UNLIKELY(state->runstate != VM_RUNNING)) return (FnWrap) { vm_halt };
  VM_ASSERT2_DEBUG(state->instr == prev_instr, "Directly called function must not change control flow!");
  state->instr = next_instr;
  STEP_VM;
}

FnWrap vm_halt(VMState *state) {
  (void) state;
  return (FnWrap) { vm_halt };
}

#include "vm/instrs/return.h"

#define VALUE_KIND ARG_SLOT
#define FN_NAME vm_instr_return_s
  #include "vm/instrs/return.h"
#undef VALUE_KIND
#undef FN_NAME


#define VALUE_KIND ARG_REFSLOT
#define FN_NAME vm_instr_return_r
  #include "vm/instrs/return.h"
#undef VALUE_KIND
#undef FN_NAME


#define VALUE_KIND ARG_VALUE
#define FN_NAME vm_instr_return_v
  #include "vm/instrs/return.h"
#undef VALUE_KIND
#undef FN_NAME


static FnWrap vm_instr_br(VMState *state) FAST_FN;
static FnWrap vm_instr_br(VMState *state) {
  BranchInstr * __restrict__ br_instr = (BranchInstr*) state->instr;
  Callframe * __restrict__ frame = state->frame;
  int blk = br_instr->blk;
  VM_ASSERT2_SLOT(blk < frame->uf->body.blocks_len, "slot numbering error");
  state->instr = (Instr*) ((char*) frame->uf->body.instrs_ptr + frame->uf->body.blocks_ptr[blk].offset);
  frame->prev_block = frame->block;
  frame->block = blk;
  STEP_VM;
}

#include "vm/instrs/test.h"

#define VALUE_KIND ARG_SLOT
  #define TARGET_KIND ARG_SLOT
  #define FN_NAME vm_instr_test_vs_ts
    #include "vm/instrs/test.h"
  #undef TARGET_KIND
  #undef FN_NAME

  #define TARGET_KIND ARG_REFSLOT
  #define FN_NAME vm_instr_test_vs_tr
    #include "vm/instrs/test.h"
  #undef TARGET_KIND
  #undef FN_NAME
#undef VALUE_KIND
#define VALUE_KIND ARG_REFSLOT
  #define TARGET_KIND ARG_SLOT
  #define FN_NAME vm_instr_test_vr_ts
    #include "vm/instrs/test.h"
  #undef TARGET_KIND
  #undef FN_NAME
  
  #define TARGET_KIND ARG_REFSLOT
  #define FN_NAME vm_instr_test_vr_tr
    #include "vm/instrs/test.h"
  #undef TARGET_KIND
  #undef FN_NAME
#undef VALUE_KIND

#include "vm/instrs/testbr.h"

#define TEST_KIND ARG_SLOT
#define FN_NAME vm_instr_testbr_s
#include "vm/instrs/testbr.h"
#undef TEST_KIND
#undef FN_NAME

#define TEST_KIND ARG_VALUE
#define FN_NAME vm_instr_testbr_v
#include "vm/instrs/testbr.h"
#undef TEST_KIND
#undef FN_NAME

#define FN_NAME vm_instr_alloc_static_object
#include "vm/instrs/alloc_static_object.h"
#undef FN_NAME

#define FN_NAME vm_instr_alloc_static_object_e0_stack
#define ENTRIES_STORED 0
#define ENTRIES_NUM 0
#define STACK true
#include "vm/instrs/alloc_static_object.h"
#undef FN_NAME
#undef ENTRIES_STORED
#undef ENTRIES_NUM
#undef STACK

#define FN_NAME vm_instr_alloc_static_object_e0_heap
#define ENTRIES_STORED 0
#define ENTRIES_NUM 0
#define STACK false
#include "vm/instrs/alloc_static_object.h"
#undef FN_NAME
#undef ENTRIES_STORED
#undef ENTRIES_NUM
#undef STACK

#define FN_NAME vm_instr_alloc_static_object_e1_stack
#define ENTRIES_STORED 1
#define ENTRIES_NUM 1
#define STACK true
#include "vm/instrs/alloc_static_object.h"
#undef FN_NAME
#undef ENTRIES_STORED
#undef ENTRIES_NUM
#undef STACK

#define FN_NAME vm_instr_alloc_static_object_e1_heap
#define ENTRIES_STORED 1
#define ENTRIES_NUM 1
#define STACK false
#include "vm/instrs/alloc_static_object.h"
#undef FN_NAME
#undef ENTRIES_STORED
#undef ENTRIES_NUM
#undef STACK

#define FN_NAME vm_instr_alloc_static_object_e2_stack
#define ENTRIES_STORED 2
#define ENTRIES_NUM 2
#define STACK true
#include "vm/instrs/alloc_static_object.h"
#undef FN_NAME
#undef ENTRIES_STORED
#undef ENTRIES_NUM
#undef STACK

#define FN_NAME vm_instr_alloc_static_object_e2_heap
#define ENTRIES_STORED 2
#define ENTRIES_NUM 2
#define STACK false
#include "vm/instrs/alloc_static_object.h"
#undef FN_NAME
#undef ENTRIES_STORED
#undef ENTRIES_NUM
#undef STACK

#define FN_NAME vm_instr_alloc_static_object_e3_stack
#define ENTRIES_STORED 3
#define ENTRIES_NUM 4
#define STACK true
#include "vm/instrs/alloc_static_object.h"
#undef FN_NAME
#undef ENTRIES_STORED
#undef ENTRIES_NUM
#undef STACK

#define FN_NAME vm_instr_alloc_static_object_e3_heap
#define ENTRIES_STORED 3
#define ENTRIES_NUM 4
#define STACK false
#include "vm/instrs/alloc_static_object.h"
#undef FN_NAME
#undef ENTRIES_STORED
#undef ENTRIES_NUM
#undef STACK

static FnWrap vm_instr_phi(VMState *state) FAST_FN;
static FnWrap vm_instr_phi(VMState *state) {
  PhiInstr * __restrict__ phi = (PhiInstr*) state->instr;
  if (state->frame->prev_block == phi->block1) {
    set_arg(state, phi->target, load_arg(state->frame, phi->arg1));
  } else if (state->frame->prev_block == phi->block2) {
    set_arg(state, phi->target, load_arg(state->frame, phi->arg2));
  } else VM_ASSERT2(false, "phi block error: arrived here from block not in list: [%i, %i], but came from %i",
                    phi->block1, phi->block2, state->frame->prev_block);
  
  state->instr = (Instr*)(phi + 1);
  STEP_VM;
}

static FnWrap vm_instr_define_refslot(VMState *state) FAST_FN;
static FnWrap vm_instr_define_refslot(VMState *state) {
  DefineRefslotInstr * __restrict__ dri = (DefineRefslotInstr*) state->instr;
  
  Refslot target_refslot = dri->target_refslot;
  Slot obj_slot = dri->obj_slot;
  
  Object *obj = OBJ_OR_NULL(read_slot(state->frame, obj_slot));
  VM_ASSERT2(obj, "cannot define refslot for null or primitive obj");
  
  TableEntry *entry = table_lookup_prepared(&obj->tbl, &dri->key);
  VM_ASSERT2(entry, "key not in object");
  *get_refslot_ref(state->frame, target_refslot) = entry;
  
  state->instr = (Instr*)(dri + 1);
  STEP_VM;
}

static FnWrap vm_instr_move(VMState *state) FAST_FN;
static FnWrap vm_instr_move(VMState *state) {
  MoveInstr * __restrict__ mi = (MoveInstr*) state->instr;
  
  set_arg(state, mi->target, load_arg(state->frame, mi->source));
  
  state->instr = (Instr*)(mi + 1);
  STEP_VM;
}

void vm_update_frame(VMState *state) {
  state->frame->instr_ptr = state->instr;
}

static void vm_step(VMState *state) {
  state->instr = state->frame->instr_ptr;
  assert(!state->frame->uf || state->frame->uf->resolved);
  
  VMInstrFn fn = state->instr->fn;
  
  // fuzz to prevent profiler aliasing
  int limit = 128 + (state->shared->cyclecount % 64);
  int i;
  for (i = 0; i < limit && fn != vm_halt; i++) {
    /*{
      fprintf(stderr, "run [%p] <%i> {%p, %i, %i} ", (void*) state->instr, state->frame->block, (void*) state->frame->uf, state->frame->uf->slots, state->frame->uf->refslots);
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
  if (state->shared->profstate.profiling_enabled) {
    vm_maybe_record_profile(state);
  }
}

void init_instr_fn_table() {
  instr_fns[INSTR_ALLOC_OBJECT] = vm_instr_alloc_object;
  instr_fns[INSTR_ALLOC_INT_OBJECT] = vm_instr_alloc_int_object;
  instr_fns[INSTR_ALLOC_BOOL_OBJECT] = vm_instr_alloc_bool_object;
  instr_fns[INSTR_ALLOC_FLOAT_OBJECT] = vm_instr_alloc_float_object;
  instr_fns[INSTR_ALLOC_ARRAY_OBJECT] = vm_instr_alloc_array_object;
  instr_fns[INSTR_ALLOC_STRING_OBJECT] = vm_instr_alloc_string_object;
  instr_fns[INSTR_ALLOC_CLOSURE_OBJECT] = vm_instr_alloc_closure_object;
  instr_fns[INSTR_FREE_OBJECT] = vm_instr_free_object;
  instr_fns[INSTR_CLOSE_OBJECT] = vm_instr_close_object;
  instr_fns[INSTR_FREEZE_OBJECT] = vm_instr_freeze_object;
  instr_fns[INSTR_ACCESS] = vm_instr_access;
  instr_fns[INSTR_ASSIGN] = vm_instr_assign;
  instr_fns[INSTR_KEY_IN_OBJ] = vm_instr_key_in_obj;
  instr_fns[INSTR_IDENTICAL] = vm_instr_identical;
  instr_fns[INSTR_INSTANCEOF] = vm_instr_instanceof;
  instr_fns[INSTR_SET_CONSTRAINT] = vm_instr_set_constraint;
  instr_fns[INSTR_TEST] = vm_instr_test;
  instr_fns[INSTR_CALL] = vm_instr_call;
  instr_fns[INSTR_RETURN] = vm_instr_return;
  instr_fns[INSTR_BR] = vm_instr_br;
  instr_fns[INSTR_TESTBR] = vm_instr_testbr;
  instr_fns[INSTR_PHI] = vm_instr_phi;
  instr_fns[INSTR_ACCESS_STRING_KEY] = vm_instr_access_string_key;
  instr_fns[INSTR_ASSIGN_STRING_KEY] = vm_instr_assign_string_key;
  instr_fns[INSTR_STRING_KEY_IN_OBJ] = vm_instr_string_key_in_obj;
  instr_fns[INSTR_SET_CONSTRAINT_STRING_KEY] = vm_instr_set_constraint_string_key;
  instr_fns[INSTR_DEFINE_REFSLOT] = vm_instr_define_refslot;
  instr_fns[INSTR_MOVE] = vm_instr_move;
  instr_fns[INSTR_CALL_FUNCTION_DIRECT] = vm_instr_call_function_direct;
  instr_fns[INSTR_ALLOC_STATIC_OBJECT] = vm_instr_alloc_static_object;
}

// will be called again after runtime optimization
void vm_resolve_functions(UserFunction *uf) {
  for (int i = 0; i < uf->body.blocks_len; i++) {
    Instr *instr_cur = BLOCK_START(uf, i), *instr_end = BLOCK_END(uf, i);
    while (instr_cur != instr_end) {
      instr_cur->fn = NULL;
      if (instr_cur->type == INSTR_TEST) {
        TestInstr *instr = (TestInstr*) instr_cur;
        if (instr->target.kind == ARG_SLOT) {
          if (instr->value.kind == ARG_SLOT) {
            instr_cur->fn = vm_instr_test_vs_ts;
          } else if (instr->value.kind == ARG_REFSLOT) {
            instr_cur->fn = vm_instr_test_vr_ts;
          };
        } else if (instr->target.kind == ARG_REFSLOT) {
          if (instr->value.kind == ARG_SLOT) {
            instr_cur->fn = vm_instr_test_vs_tr;
          } else if (instr->value.kind == ARG_REFSLOT) {
            instr_cur->fn = vm_instr_test_vr_tr;
          }
        }
      } else if (instr_cur->type == INSTR_TESTBR) {
        TestBranchInstr *instr = (TestBranchInstr*) instr_cur;
        if (instr->test.kind == ARG_SLOT) {
          instr_cur->fn = vm_instr_testbr_s;
        } else if (instr->test.kind == ARG_VALUE) {
          instr_cur->fn = vm_instr_testbr_v;
        }
      } else if (instr_cur->type == INSTR_RETURN) {
        ReturnInstr *instr = (ReturnInstr*) instr_cur;
        if (instr->ret.kind == ARG_SLOT) {
          instr_cur->fn = vm_instr_return_s;
        } else if (instr->ret.kind == ARG_REFSLOT) {
          instr_cur->fn = vm_instr_return_r;
        } else if (instr->ret.kind == ARG_VALUE) {
          instr_cur->fn = vm_instr_return_v;
        }
      } else if (instr_cur->type == INSTR_CALL_FUNCTION_DIRECT) {
        CallFunctionDirectInstr *instr = (CallFunctionDirectInstr*) instr_cur;
        if (instr->fast) {
          instr_cur->fn = instr->dispatch_fn(instr_cur).self;
        }
      } else if (instr_cur->type == INSTR_ALLOC_STATIC_OBJECT) {
        AllocStaticObjectInstr *instr = (AllocStaticObjectInstr*) instr_cur;
        if (instr->alloc_stack) {
          if (instr->tbl.entries_stored == 0) {
            instr_cur->fn = vm_instr_alloc_static_object_e0_stack;
          } else if (instr->tbl.entries_stored == 1) {
            instr_cur->fn = vm_instr_alloc_static_object_e1_stack;
          } else if (instr->tbl.entries_stored == 2) {
            instr_cur->fn = vm_instr_alloc_static_object_e2_stack;
          } else if (instr->tbl.entries_stored == 3) {
            instr_cur->fn = vm_instr_alloc_static_object_e3_stack;
          }
        } else {
          if (instr->tbl.entries_stored == 0) {
            instr_cur->fn = vm_instr_alloc_static_object_e0_heap;
          } else if (instr->tbl.entries_stored == 1) {
            instr_cur->fn = vm_instr_alloc_static_object_e1_heap;
          } else if (instr->tbl.entries_stored == 2) {
            instr_cur->fn = vm_instr_alloc_static_object_e2_heap;
          } else if (instr->tbl.entries_stored == 3) {
            instr_cur->fn = vm_instr_alloc_static_object_e3_heap;
          }
        }
      }
      if (!instr_cur->fn) instr_cur->fn = instr_fns[instr_cur->type];
      int size = instr_size(instr_cur);
      instr_cur = (Instr*) ((char*) instr_cur + size);
    }
  }
}

void vm_resolve(UserFunction *uf) {
  assert(!uf->resolved);
  vm_resolve_functions(uf);
  for (int i = 0; i < uf->body.blocks_len; i++) {
    Instr *instr_cur = BLOCK_START(uf, i), *instr_end = BLOCK_END(uf, i);
    while (instr_cur != instr_end) {
      switch (instr_cur->type) {
#define CASE(KEY, TY) } break; case KEY: { TY *instr = (TY*) instr_cur; (void) instr;
#define CHKSLOT_READ(S) resolve_slot_ref(uf, &S)
#define CHKSLOT_WRITE(S) resolve_slot_ref(uf, &S)
#define CHKSLOT_REF(S) resolve_refslot_ref(uf, &S)
        case INSTR_INVALID: { abort();
#include "vm/slots.txt"
          CASE(INSTR_LAST, Instr) abort();
        } break;
        default: assert("Unhandled Instruction Type!" && false);
#undef CHKSLOT_READ
#undef CHKSLOT_WRITE
#undef CHKSLOT_REF
#undef CASE
      }
      int size = instr_size(instr_cur);
      instr_cur = (Instr*) ((char*) instr_cur + size);
    }
  }
  uf->resolved = true;
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
    if (state->shared->gcstate.bytes_allocated > state->shared->gcstate.next_gc_run) {
      // fprintf(stderr, "allocated %i, next_gc_run %i\n", state->shared->gcstate.bytes_allocated, state->shared->gcstate.next_gc_run);
      gc_run(state);
      // run gc after 50% growth or 10000000 allocated or thereabouts
      state->shared->gcstate.next_gc_run = (int) (state->shared->gcstate.bytes_allocated * 1.5) + 10000000; // don't even get out of bed for less than 10MB
      // fprintf(stderr, "left over %i, set next to %i\n", state->shared->gcstate.bytes_allocated, state->shared->gcstate.next_gc_run);
    }
  }
}

void vm_setup_substate_of(VMState *substate, VMState *state) {
  *substate = (VMState) {0};
  substate->runstate = VM_TERMINATED;
  substate->parent = state;
  substate->root = state->root;
  substate->shared = state->shared;
}
