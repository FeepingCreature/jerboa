#include "print.h"
#include "vm/vm.h"
#include <stdio.h>

static void print_recursive_indent(VMState *state, Object *obj, bool allow_tostring, int indent) {
  Object *root = state->root;
  if (obj == NULL) {
    printf("(null)");
    return;
  }
  Object *int_base = state->shared->vcache.int_base;
  Object *bool_base = state->shared->vcache.bool_base;
  Object *float_base = state->shared->vcache.float_base;
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
      print_recursive(state, child, allow_tostring);
      if (state->runstate == VM_ERRORED) return;
    }
    printf(" ]");
    return;
  }
  Object *toString_fn = object_lookup(obj, "toString", NULL);
  if (allow_tostring && toString_fn) {
    Object *function_base = state->shared->vcache.function_base;
    Object *closure_base = state->shared->vcache.closure_base;
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
      print_recursive(state, str, allow_tostring);
      gc_enable(state);
      return;
    }
  }
  printf("[object %p ", (void*) obj);
  if (obj->flags == OBJ_NONE) { }
  else {
    printf("(");
    if (obj->flags & OBJ_CLOSED) {
      printf("CLS");
      if (obj->flags & (OBJ_FROZEN|OBJ_NOINHERIT)) printf("|");
    }
    if (obj->flags & OBJ_FROZEN) {
      printf("FRZ");
      if (obj->flags & OBJ_NOINHERIT) printf("|");
    }
    if (obj->flags & OBJ_NOINHERIT) printf("NOI");
    printf(")");
  }
  HashTable *tbl = &obj->tbl;
  bool first = true;
  for (int i = 0; i < tbl->entries_num; ++i) {
    TableEntry *entry = &tbl->entries_ptr[i];
    if (entry->name_ptr) {
      printf("\n");
      for (int k = 0; k < indent; ++k) printf("  ");
      if (first) { first = false; printf("| "); }
      else printf(", ");
      printf("'%.*s': ", (int) entry->name_len, entry->name_ptr);
      print_recursive_indent(state, entry->value, allow_tostring, indent+1);
    }
  }
  if (obj->parent) {
    printf("\n");
    for (int k = 0; k < indent; ++k) printf("  ");
    printf("<- ");
    print_recursive_indent(state, obj->parent, allow_tostring, indent+1);
  }
  printf("]");
  // vm_error(state, "don't know how to print %p", obj);
  return;
}

void print_recursive(VMState *state, Object *obj, bool allow_tostring) {
  print_recursive_indent(state, obj, allow_tostring, 0);
}
