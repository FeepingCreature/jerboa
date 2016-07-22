#include "print.h"
#include "vm/vm.h"
#include "vm/call.h"
#include <stdio.h>

static void print_recursive_indent(VMState *state, FILE *fh, Object *obj, bool allow_tostring, int indent) {
  if (obj == NULL) {
    fprintf(fh, "(null)");
    return;
  }
  Object *int_base = state->shared->vcache.int_base; assert(int_base->flags & OBJ_NOINHERIT);
  Object *bool_base = state->shared->vcache.bool_base; assert(bool_base->flags & OBJ_NOINHERIT);
  Object *float_base = state->shared->vcache.float_base; assert(float_base->flags & OBJ_NOINHERIT);
  Object *array_base = state->shared->vcache.array_base;
  Object *string_base = state->shared->vcache.string_base;
  Object *pointer_base = state->shared->vcache.pointer_base;
  Object *function_base = state->shared->vcache.function_base;
  Object
    *sobj = obj_instance_of(obj, string_base),
    *aobj = obj_instance_of(obj, array_base),
    *fobj = obj_instance_of(obj, function_base);
  if (obj->parent == int_base) {
    fprintf(fh, "%i", obj->int_value);
    return;
  }
  if (obj->parent == bool_base) {
    if (obj->bool_value) fprintf(fh, "true");
    else fprintf(fh, "false");
    return;
  }
  if (obj->parent == float_base) {
    fprintf(fh, "%f", obj->float_value);
    return;
  }
  if (sobj) {
    fprintf(fh, "%s", ((StringObject*)sobj)->value);
    return;
  }
  if (fobj) {
    fprintf(fh, "<function %p>", *(void**) &((FunctionObject*)fobj)->fn_ptr);
    return;
  }
  if (obj->parent == pointer_base) {
    fprintf(fh, "(void*) %p", ((PointerObject*)obj)->ptr);
    return;
  }
  if (aobj) {
    fprintf(fh, "[");
    ArrayObject *a_obj = (ArrayObject*) aobj;
    for (int i = 0; i < a_obj->length; ++i) {
      Object *child = a_obj->ptr[i];
      if (i) fprintf(fh, ", ");
      else fprintf(fh, " ");
      print_recursive(state, fh, child, allow_tostring);
      if (state->runstate == VM_ERRORED) return;
    }
    fprintf(fh, " ]");
    return;
  }
  Object *toString_fn = object_lookup(obj, "toString", NULL);
  if (allow_tostring && toString_fn) {
    VMState substate = {0};
    substate.parent = state;
    substate.root = state->root;
    substate.shared = state->shared;
    
    if (!setup_call(&substate, obj, toString_fn, NULL, 0)) return;
    
    vm_run(&substate);
    VM_ASSERT(substate.runstate != VM_ERRORED, "toString failure: %s\n", substate.error);
    
    Object *str = substate.exit_value;
    if (str != NULL) {
      gc_disable(state); // keep str alive
      print_recursive(state, fh, str, allow_tostring);
      gc_enable(state);
      return;
    }
  }
  fprintf(fh, "[object %p ", (void*) obj);
  if (obj->flags == OBJ_NONE) { }
  else {
    fprintf(fh, "(");
    if (obj->flags & OBJ_CLOSED) {
      fprintf(fh, "CLS");
      if (obj->flags & (OBJ_FROZEN|OBJ_NOINHERIT)) fprintf(fh, "|");
    }
    if (obj->flags & OBJ_FROZEN) {
      fprintf(fh, "FRZ");
      if (obj->flags & OBJ_NOINHERIT) fprintf(fh, "|");
    }
    if (obj->flags & OBJ_NOINHERIT) fprintf(fh, "NOI");
    fprintf(fh, ")");
  }
  if (!(obj->flags & OBJ_PRIMITIVE)) {
    HashTable *tbl = &obj->tbl;
    bool first = true;
    for (int i = 0; i < tbl->entries_num; ++i) {
      TableEntry *entry = &tbl->entries_ptr[i];
      if (entry->name_ptr) {
        fprintf(fh, "\n");
        for (int k = 0; k < indent; ++k) fprintf(fh, "  ");
        if (first) { first = false; fprintf(fh, "| "); }
        else fprintf(fh, ", ");
        fprintf(fh, "'%.*s': ", (int) entry->name_len, entry->name_ptr);
        print_recursive_indent(state, fh, entry->value, allow_tostring, indent+1);
        if (state->runstate == VM_ERRORED) return;
      }
    }
  }
  if (obj->parent) {
    fprintf(fh, "\n");
    for (int k = 0; k < indent; ++k) fprintf(fh, "  ");
    fprintf(fh, "<- ");
    print_recursive_indent(state, fh, obj->parent, allow_tostring, indent+1);
    if (state->runstate == VM_ERRORED) return;
  }
  fprintf(fh, "]");
  // vm_error(state, "don't know how to print %p", obj);
  return;
}

void print_recursive(VMState *state, FILE *fh, Object *obj, bool allow_tostring) {
  print_recursive_indent(state, fh, obj, allow_tostring, 0);
}
