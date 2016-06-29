#include "print.h"
#include "vm/vm.h"
#include <stdio.h>

void print_recursive(VMState *state, Object *obj) {
  Object *root = state->root;
  if (obj == NULL) {
    printf("(null)");
    return;
  }
  Object *int_base = object_lookup(root, "int", NULL);
  Object *bool_base = object_lookup(root, "bool", NULL);
  Object *float_base = object_lookup(root, "float", NULL);
  Object *array_base = object_lookup(root, "array", NULL);
  Object *string_base = object_lookup(root, "string", NULL);
  Object
    *iobj = obj_instance_of(obj, int_base),
    *bobj = obj_instance_of(obj, bool_base),
    *fobj = obj_instance_of(obj, float_base),
    *sobj = obj_instance_of(obj, string_base),
    *aobj = obj_instance_of(obj, array_base);
  if (iobj) {
    printf("%i", ((IntObject*)iobj)->value);
    return;
  }
  if (bobj) {
    if (((BoolObject*)bobj)->value) printf("true");
    else printf("false");
    return;
  }
  if (fobj) {
    printf("%f", ((FloatObject*)fobj)->value);
    return;
  }
  if (sobj) {
    printf("%s", ((StringObject*)sobj)->value);
    return;
  }
  if (aobj) {
    printf("[ ");
    ArrayObject *a_obj = (ArrayObject*) aobj;
    for (int i = 0; i < a_obj->length; ++i) {
      Object *child = a_obj->ptr[i];
      if (i) printf(", ");
      print_recursive(state, child);
      if (state->runstate == VM_ERRORED) return;
    }
    printf(" ]");
    return;
  }
  Object *toString_fn = object_lookup(obj, "toString", NULL);
  if (toString_fn) {
    Object *function_base = object_lookup(root, "function", NULL);
    Object *closure_base = object_lookup(root, "closure", NULL);
    FunctionObject *fn_toString = (FunctionObject*) obj_instance_of(toString_fn, function_base);
    ClosureObject *cl_toString = (ClosureObject*) obj_instance_of(toString_fn, closure_base);
    VM_ASSERT(fn_toString || cl_toString, "'toString' property is neither function nor closure");
    
    VMState substate = {0};
    substate.parent = state;
    substate.root = state->root;
    substate.shared = state->shared;
    
    if (fn_toString) fn_toString->fn_ptr(&substate, obj, toString_fn, NULL, 0);
    else cl_toString->base.fn_ptr(&substate, obj, toString_fn, NULL, 0);
    
    vm_run(&substate);
    VM_ASSERT(substate.runstate != VM_ERRORED, "toString failure: %s\n", substate.error);
    
    Object *str = substate.result_value;
    if (str != NULL) {
      gc_disable(state); // keep str alive
      print_recursive(state, str);
      gc_enable(state);
      return;
    }
  }
  printf("[object %p", (void*) obj);
  HashTable *tbl = &obj->tbl;
  bool first = true;
  for (int i = 0; i < tbl->entries_num; ++i) {
    TableEntry *entry = &tbl->entries_ptr[i];
    if (entry->name_ptr) {
      if (first) { first = false; printf(" | "); }
      else printf(", ");
      printf("'%.*s': ", (int) entry->name_len, entry->name_ptr);
      print_recursive(state, entry->value);
    }
  }
  printf("]");
  // vm_error(state, "don't know how to print %p", obj);
  return;
}
