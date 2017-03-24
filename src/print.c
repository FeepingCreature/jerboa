#include "print.h"
#include "vm/vm.h"
#include "vm/call.h"
#include <stdio.h>

static void print_recursive_indent(VMState *state, FILE *fh, Value val, bool allow_tostring, int indent) {
  if (IS_NULL(val)) {
    fprintf(fh, "(null)");
    return;
  }
  Object *array_base = state->shared->vcache.array_base;
  Object *string_base = state->shared->vcache.string_base;
  Object *pointer_base = state->shared->vcache.pointer_base;
  Object *function_base = state->shared->vcache.function_base;
  Object *obj = OBJ_OR_NULL(val);
  Object
    *sobj = obj_instance_of(obj, string_base),
    *aobj = obj_instance_of(obj, array_base),
    *fobj = obj_instance_of(obj, function_base);
  if (IS_INT(val)) {
    fprintf(fh, "%i", AS_INT(val));
    return;
  }
  if (IS_BOOL(val)) {
    if (AS_BOOL(val)) fprintf(fh, "true");
    else fprintf(fh, "false");
    return;
  }
  if (IS_FLOAT(val)) {
    fprintf(fh, "%f", AS_FLOAT(val));
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
  if (obj->flags & OBJ_PRINT_HACK) {
    fprintf(fh, "already printed!");
    return;
  }
  obj->flags |= OBJ_PRINT_HACK;
  if (aobj) {
    fprintf(fh, "[");
    ArrayObject *a_obj = (ArrayObject*) aobj;
    for (int i = 0; i < a_obj->length; ++i) {
      Value child = a_obj->ptr[i];
      if (i) fprintf(fh, ", ");
      else fprintf(fh, " ");
      print_recursive_indent(state, fh, child, allow_tostring, indent + 1);
      if (state->runstate == VM_ERRORED) return;
    }
    fprintf(fh, " ]");
    obj->flags &= ~OBJ_PRINT_HACK;
    return;
  }
  Value toString_fn = OBJECT_LOOKUP_STRING(obj, "toString");
  if (allow_tostring && NOT_NULL(toString_fn)) {
    VMState substate;
    vm_setup_substate_of(&substate, state);
    
    Value str;
    
    CallInfo info = {{0}};
    info.this_arg = (Arg) { .kind = ARG_VALUE, .value = val };
    info.fn = (Arg) { .kind = ARG_VALUE, .value = toString_fn };
    info.target = (WriteArg) { .kind = ARG_POINTER, .pointer = &str };
    
    if (!setup_call(&substate, &info, NULL)) return;
    
    vm_update_frame(&substate);
    vm_run(&substate);
    VM_ASSERT(substate.runstate != VM_ERRORED, "toString failure: %s\n", substate.error);
    
    if (NOT_NULL(str)) {
      print_recursive(state, fh, str, allow_tostring);
    }
    obj->flags &= ~OBJ_PRINT_HACK;
    return;
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
  HashTable *tbl = &obj->tbl;
  bool first = true;
  for (int i = 0; i < tbl->entries_num; ++i) {
    TableEntry *entry = &tbl->entries_ptr[i];
    if (entry->key_ptr) {
      fprintf(fh, "\n");
      for (int k = 0; k < indent; ++k) fprintf(fh, "  ");
      if (first) { first = false; fprintf(fh, "| "); }
      else fprintf(fh, ", ");
      fprintf(fh, "'%s': ", entry->key_ptr);
      print_recursive_indent(state, fh, entry->value, allow_tostring, indent+1);
      if (state->runstate == VM_ERRORED) return;
    }
  }
  if (obj->parent) {
    fprintf(fh, "\n");
    for (int k = 0; k < indent; ++k) fprintf(fh, "  ");
    fprintf(fh, "<- ");
    print_recursive_indent(state, fh, OBJ2VAL(obj->parent), allow_tostring, indent+1);
    if (state->runstate == VM_ERRORED) return;
  }
  fprintf(fh, "]");
  // vm_error(state, "don't know how to print %p", obj);
  obj->flags &= ~OBJ_PRINT_HACK;
  return;
}

void print_recursive(VMState *state, FILE *fh, Value val, bool allow_tostring) {
  gc_disable(state);
  print_recursive_indent(state, fh, val, allow_tostring, 0);
  gc_enable(state);
}
