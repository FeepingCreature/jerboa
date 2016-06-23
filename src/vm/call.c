#include "call.h"

#include "vm/vm.h"
#include "util.h"
#include "gc.h"

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
  cf->instr_ptr = cf->uf->body.blocks_ptr[0].instrs_ptr;
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
