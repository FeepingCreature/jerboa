#include "vm/call.h"

#include "vm/vm.h"
#include "util.h"
#include "gc.h"

void call_function(VMState *state, Object *context, UserFunction *fn, CallInfo *info) {
  Callframe *callf = state->frame;
  vm_alloc_frame(state, fn->slots, fn->refslots);
  Callframe *cf = state->frame;
  cf->uf = fn;
  cf->slots_ptr[1] = OBJ2VAL(context);
  cf->target = info->target;
  
  if (fn->variadic_tail) {
    if (info->args_len < fn->arity) { vm_error(state, "arity violation in call!"); return; }
  } else {
    if (info->args_len != fn->arity) { vm_error(state, "arity violation in call!"); return; }
  }
  for (int i = 0; i < info->args_len; ++i) {
    cf->slots_ptr[2 + i] = load_arg(callf, INFO_ARGS_PTR(info)[i]);
  }
  
  if (fn->body.blocks_len == 0) {
    vm_error(state, "invalid function: no instructions");
    return;
  }
  state->instr = fn->body.instrs_ptr;
}

#include "vm/optimize.h"
void call_closure(VMState *state, Object *context, ClosureObject *cl, CallInfo *info) {
  cl->num_called ++;
  if (UNLIKELY(cl->num_called == 10)) {
    assert(!cl->vmfun->optimized);
    cl->vmfun = optimize_runtime(state, cl->vmfun, context);
    assert(cl->vmfun->optimized);
  }
  call_function(state, context, cl->vmfun, info);
}

static Object *setup_vararg(VMState *state, Object *context, UserFunction *uf, CallInfo *info) {
  if (!uf->variadic_tail) return context;
  context = AS_OBJ(make_object(state, context));
  // should have been checked before
  assert(info->args_len >= uf->arity);
  int varargs_len = info->args_len - uf->arity;
  // when/how is this actually freed??
  // TODO owned_array?
  Value *varargs_ptr = malloc(sizeof(Value) * varargs_len);
  for (int i = 0; i < varargs_len; ++i) {
    varargs_ptr[i] = load_arg(state->frame, INFO_ARGS_PTR(info)[uf->arity + i]);
  }
  object_set(state, context, "arguments", make_array(state, varargs_ptr, varargs_len));
  context->flags |= OBJ_CLOSED;
  return context;
}

static void function_handler(VMState *state, ClosureObject *cl_obj, CallInfo *info) {
  Object *context = cl_obj->context;
  gc_disable(state); // keep context alive, if need be
  context = setup_vararg(state, context, cl_obj->vmfun, info);
  call_closure(state, context, cl_obj, info);
  gc_enable(state);
}

static void method_handler(VMState *state, ClosureObject *cl_obj, CallInfo *info) {
  Object *context = AS_OBJ(make_object(state, cl_obj->context));
  create_table_with_single_entry(&context->tbl, "this", 4, hash("this", 4), load_arg(state->frame, info->this_arg));
  context->flags |= OBJ_CLOSED;
  gc_disable(state); // keep context alive
  context = setup_vararg(state, context, cl_obj->vmfun, info);
  call_closure(state, context, cl_obj, info);
  gc_enable(state);
}

static void closure_mark_fn(VMState *state, Object *obj) {
  Object *closure_base = state->shared->vcache.closure_base;
  ClosureObject *clobj = (ClosureObject*) obj_instance_of(obj, closure_base);
  if (clobj) obj_mark(state, clobj->context);
}

Value make_closure_fn(VMState *state, Object *context, UserFunction *fn) {
  ClosureObject *obj = alloc_object_internal(state, sizeof(ClosureObject));
  obj->base.parent = state->shared->vcache.closure_base;
  obj->base.mark_fn = closure_mark_fn;
  obj->context = context;
  obj->vmfun = fn;
  return OBJ2VAL((Object*) obj);
}

bool setup_call(VMState *state, CallInfo *info) {
  Object *function_base = state->shared->vcache.function_base;
  Value fn = load_arg(state->frame, info->fn);
  VM_ASSERT(IS_OBJ(fn), "this is not a thing I can call.") false;
  Object *fn_obj_n = AS_OBJ(fn);
  if (fn_obj_n->parent == function_base) {
    ((FunctionObject*)fn_obj_n)->fn_ptr(state, info);
    return state->runstate != VM_ERRORED;
  } else {
    Object *closure_base = state->shared->vcache.closure_base;
    ClosureObject *cl_obj = (ClosureObject*) obj_instance_of(fn_obj_n, closure_base);
    if (cl_obj) {
      UserFunction *vmfun = cl_obj->vmfun;
      if (vmfun->is_method) method_handler(state, cl_obj, info);
      else function_handler(state, cl_obj, info);
      return state->runstate != VM_ERRORED;
    } else {
      VM_ASSERT(false, "object is neither function nor closure") false;
    }
  }
}
