#include "vm/call.h"

#include "vm/vm.h"
#include "util.h"
#include "gc.h"

void call_function(VMState *state, Object *context, UserFunction *fn, Object **args_ptr, int args_len) {
  Callframe *cf = vm_alloc_frame(state, fn->slots, fn->refslots);
  
  cf->uf = fn;
  cf->slots_ptr[1] = context;
  gc_add_roots(state, cf->slots_ptr, cf->slots_len, &cf->frameroot_slots);
  
  if (fn->variadic_tail) {
    if (args_len < cf->uf->arity) { vm_error(state, "arity violation in call!"); return; }
  } else {
    if (args_len != cf->uf->arity) { vm_error(state, "arity violation in call!"); return; }
  }
  for (int i = 0; i < args_len; ++i) {
    cf->slots_ptr[2 + i] = args_ptr[i];
  }
  
  if (cf->uf->body.blocks_len == 0) {
    vm_error(state, "invalid function: no instructions");
    return;
  }
  cf->instr_ptr = cf->uf->body.blocks_ptr[0].instrs_ptr;
}

#include "vm/optimize.h"
void call_closure(VMState *state, Object *context, ClosureObject *cl, Object **args_ptr, int args_len) {
  cl->num_called ++;
  if (cl->num_called == 10) {
    cl->vmfun = optimize_runtime(state, cl->vmfun, context);
  }
  call_function(state, context, cl->vmfun, args_ptr, args_len);
}

static Object *setup_vararg(VMState *state, Object *context, UserFunction *uf, Object **args_ptr, int args_len) {
  if (!uf->variadic_tail) return context;
  context = alloc_object(state, context);
  // should have been checked before
  assert(args_len >= uf->arity);
  int varargs_len = args_len - uf->arity;
  // TODO when/how is this actually freed??
  // TODO owned_array?
  Object **varargs_ptr = malloc(sizeof(Object*) * varargs_len);
  for (int i = 0; i < varargs_len; ++i) {
    varargs_ptr[i] = args_ptr[uf->arity + i];
  }
  object_set(context, "arguments", alloc_array(state, varargs_ptr, (IntObject*) alloc_int(state, varargs_len)));
  context->flags |= OBJ_CLOSED;
  return context;
}

static void function_handler(VMState *state, Object *thisptr, Object *fn, Object **args_ptr, int args_len) {
  // discard calling context (lexical scoping!)
  ClosureObject *cl_obj = (ClosureObject*) fn;
  Object *context = cl_obj->context;
  gc_disable(state); // keep context alive, if need be
  context = setup_vararg(state, context, cl_obj->vmfun, args_ptr, args_len);
  call_closure(state, context, cl_obj, args_ptr, args_len);
  gc_enable(state);
}

static void method_handler(VMState *state, Object *thisptr, Object *fn, Object **args_ptr, int args_len) {
  // discard calling context (lexical scoping!)
  ClosureObject *cl_obj = (ClosureObject*) fn;
  Object *context = alloc_object(state, cl_obj->context);
  object_set(context, "this", thisptr);
  context->flags |= OBJ_CLOSED;
  gc_disable(state); // keep context alive
  context = setup_vararg(state, context, cl_obj->vmfun, args_ptr, args_len);
  call_closure(state, context, cl_obj, args_ptr, args_len);
  gc_enable(state);
}

Object *alloc_closure_fn(VMState *state, Object *context, UserFunction *fn) {
  ClosureObject *obj = calloc(sizeof(ClosureObject), 1);
  obj->base.base.parent = state->shared->vcache.closure_base;
  if (fn->is_method) obj->base.fn_ptr = method_handler;
  else obj->base.fn_ptr = function_handler;
  obj->context = context;
  obj->vmfun = fn;
  return (Object*) obj;
}

bool setup_call(VMState *state, Object *thisptr, Object *fn, Object **args_ptr, int args_len) {
  Object *closure_base = state->shared->vcache.closure_base;
  Object *function_base = state->shared->vcache.function_base;
  FunctionObject *fn_obj = (FunctionObject*) obj_instance_of(fn, function_base);
  ClosureObject *cl_obj = (ClosureObject*) obj_instance_of(fn, closure_base);
  VM_ASSERT(fn_obj || cl_obj, "object is neither function nor closure") false;
  
  if (fn_obj) fn_obj->fn_ptr(state, thisptr, fn, args_ptr, args_len);
  else cl_obj->base.fn_ptr(state, thisptr, fn, args_ptr, args_len);
  return true;
}
