#include "vm/call.h"

#include "vm/optimize.h"
#include "vm/vm.h"
#include "util.h"
#include "gc.h"

void vm_resolve(UserFunction *uf);

void call_function(VMState *state, Object *context, UserFunction *fn, CallInfo *info) {
  if (UNLIKELY(!fn->resolved)) vm_resolve(fn);
  Callframe *callf = state->frame;
  vm_alloc_frame(state, fn->slots, fn->refslots);
  Callframe *cf = state->frame;
  cf->uf = fn;
  cf->slots_ptr[1] = OBJ2VAL(context);
  cf->target = info->target;
  
  // enforced in build_function
  assert(fn->body.blocks_len > 0);
  // do this early so that the backtrace contains the called function
  state->instr = fn->body.instrs_ptr;
  
  if (UNLIKELY(fn->variadic_tail)) {
    VM_ASSERT(info->args_len >= fn->arity, "arity violation in call!");
  } else {
    VM_ASSERT(info->args_len == fn->arity, "arity violation in call!");
  }
  
  for (int i = 0; i < info->args_len; ++i) {
    cf->slots_ptr[2 + i] = load_arg(callf, INFO_ARGS_PTR(info)[i]);
  }
}

static void closure_mark_fn(VMState *state, Object *obj) {
  Object *closure_base = state->shared->vcache.closure_base;
  ClosureObject *clobj = (ClosureObject*) obj_instance_of(obj, closure_base);
  if (clobj) obj_mark(state, clobj->context);
}

Value make_closure_fn(VMState *state, Object *context, UserFunction *fn) {
  ClosureObject *obj = alloc_object_internal(state, sizeof(ClosureObject), false);
  obj->base.parent = state->shared->vcache.closure_base;
  obj->base.mark_fn = closure_mark_fn;
  obj->context = context;
  obj->vmfun = fn;
  obj->num_called = 0;
  return OBJ2VAL((Object*) obj);
}

bool setup_call(VMState *state, CallInfo *info) {
  Object *function_base = state->shared->vcache.function_base;
  Value fn = load_arg(state->frame, info->fn);
  VM_ASSERT(IS_OBJ(fn), "this is not a thing I can call.") false;
  Object *fn_obj_n = AS_OBJ(fn);
  // should have been handled outside setup_call
  // primarily to do the whole if (instr != previous instr) tango
  // (see vm.c whenever we call something)
  (void) function_base; assert(fn_obj_n->parent != function_base);
  /*
  if (fn_obj_n->parent == function_base) {
    ((FunctionObject*)fn_obj_n)->fn_ptr(state, info);
    return state->runstate != VM_ERRORED;
  }
  */
  
  Object *closure_base = state->shared->vcache.closure_base;
  ClosureObject *cl_obj = (ClosureObject*) obj_instance_of(fn_obj_n, closure_base);
  
  VM_ASSERT(cl_obj, "object is neither function nor closure") false;
  
  UserFunction *vmfun = cl_obj->vmfun;
  Object *context = cl_obj->context;
  if (UNLIKELY(state->shared->vcache.thiskey.hash == 0)) {
    state->shared->vcache.thiskey = prepare_key("this", 4);
  }
  if (vmfun->is_method) {
    context = AS_OBJ(make_object(state, context, false));
    create_table_with_single_entry_prepared(&context->tbl, state->shared->vcache.thiskey, load_arg(state->frame, info->this_arg));
    context->flags |= OBJ_CLOSED;
  }
  // gc only runs in the main loop
  // gc_disable(state); // keep context alive, if need be
  if (UNLIKELY(vmfun->variadic_tail)) {
    context = AS_OBJ(make_object(state, context, false));
    // should have been checked before
    assert(info->args_len >= vmfun->arity);
    int varargs_len = info->args_len - vmfun->arity;
    // when/how is this actually freed??
    // TODO owned_array?
    Value *varargs_ptr = malloc(sizeof(Value) * varargs_len);
    for (int i = 0; i < varargs_len; ++i) {
      varargs_ptr[i] = load_arg(state->frame, INFO_ARGS_PTR(info)[vmfun->arity + i]);
    }
    OBJECT_SET_STRING(state, context, "arguments", make_array(state, varargs_ptr, varargs_len, true));
    context->flags |= OBJ_CLOSED;
  }
  cl_obj->num_called ++;
  if (UNLIKELY(cl_obj->num_called == 10)) {
    assert(!vmfun->optimized);
    vmfun = cl_obj->vmfun = optimize_runtime(state, vmfun, context);
  }
  call_function(state, context, vmfun, info);
  // gc_enable(state);
  return state->runstate != VM_ERRORED;
}
