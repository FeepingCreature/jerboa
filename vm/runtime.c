#include "vm/runtime.h"

Object *equals(Object *context, Object *fn, Object **args_ptr, int args_len) {
  assert(args_len == 2);
  Object *root = context;
  while (root->parent) root = root->parent;
  Object *int_base = object_lookup(root, "int");
  
  Object *obj1 = args_ptr[0], *obj2 = args_ptr[1];
  if (obj1->parent == int_base && obj2->parent == int_base) {
    int test = ((IntObject*) obj1)->value == ((IntObject*) obj2)->value;
    return obj_claimed(alloc_bool(context, test?true:false));
  }
  assert(false);
}

Object *add(Object *context, Object *fn, Object **args_ptr, int args_len) {
  assert(args_len == 2);
  Object *root = context;
  while (root->parent) root = root->parent;
  Object *int_base = object_lookup(root, "int");
  
  Object *obj1 = args_ptr[0], *obj2 = args_ptr[1];
  if (obj1->parent == int_base && obj2->parent == int_base) {
    int res = ((IntObject*) obj1)->value + ((IntObject*) obj2)->value;
    return obj_claimed(alloc_int(context, res));
  }
  assert(false);
}

Object *sub(Object *context, Object *fn, Object **args_ptr, int args_len) {
  assert(args_len == 2);
  Object *root = context;
  while (root->parent) root = root->parent;
  Object *int_base = object_lookup(root, "int");
  
  Object *obj1 = args_ptr[0], *obj2 = args_ptr[1];
  if (obj1->parent == int_base && obj2->parent == int_base) {
    int res = ((IntObject*) obj1)->value - ((IntObject*) obj2)->value;
    return obj_claimed(alloc_int(context, res));
  }
  assert(false);
}

Object *mul(Object *context, Object *fn, Object **args_ptr, int args_len) {
  assert(args_len == 2);
  Object *root = context;
  while (root->parent) root = root->parent;
  Object *int_base = object_lookup(root, "int");
  
  Object *obj1 = args_ptr[0], *obj2 = args_ptr[1];
  if (obj1->parent == int_base && obj2->parent == int_base) {
    int res = ((IntObject*) obj1)->value * ((IntObject*) obj2)->value;
    return obj_claimed(alloc_int(context, res));
  }
  assert(false);
}

Object *create_root() {
  Object *root = alloc_object(NULL);
  object_set(root, "int", alloc_object(NULL));
  object_set(root, "bool", alloc_object(NULL));
  object_set(root, "function", alloc_object(NULL));
  object_set(root, "=", alloc_fn(root, equals));
  object_set(root, "+", alloc_fn(root, add));
  object_set(root, "-", alloc_fn(root, sub));
  object_set(root, "*", alloc_fn(root, mul));
  return root;
}
