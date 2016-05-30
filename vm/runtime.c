#include "vm/runtime.h"

Object *equals(Object *context, Object *fn, Object **args_ptr, int args_len) {
  assert(args_len == 2);
  Object *root = context;
  while (root->parent) root = root->parent;
  Object *int_base = object_lookup(root, "int");
  Object *float_base = object_lookup(root, "float");
  
  Object *obj1 = args_ptr[0], *obj2 = args_ptr[1];
  if (obj1->parent == int_base && obj2->parent == int_base) {
    int test = ((IntObject*) obj1)->value == ((IntObject*) obj2)->value;
    return obj_claimed(alloc_bool(context, test?true:false));
  }
  if ((obj1->parent == float_base || obj1->parent == int_base) && (obj2->parent == float_base || obj2->parent == int_base)) {
    float v1, v2;
    if (obj1->parent == float_base) v1 = ((FloatObject*) obj1)->value;
    else v1 = ((IntObject*) obj1)->value;
    if (obj2->parent == float_base) v2 = ((FloatObject*) obj2)->value;
    else v2 = ((IntObject*) obj2)->value;
    return obj_claimed(alloc_bool(context, v1 == v2));
  }
  assert(false);
}

Object *smaller(Object *context, Object *fn, Object **args_ptr, int args_len) {
  assert(args_len == 2);
  Object *root = context;
  while (root->parent) root = root->parent;
  Object *int_base = object_lookup(root, "int");
  Object *float_base = object_lookup(root, "float");
  
  Object *obj1 = args_ptr[0], *obj2 = args_ptr[1];
  if (obj1->parent == int_base && obj2->parent == int_base) {
    int i1 = ((IntObject*) obj1)->value, i2 = ((IntObject*) obj2)->value;
    return obj_claimed(alloc_bool(context, i1 < i2));
  }
  if ((obj1->parent == float_base || obj1->parent == int_base) && (obj2->parent == float_base || obj2->parent == int_base)) {
    float v1, v2;
    if (obj1->parent == float_base) v1 = ((FloatObject*) obj1)->value;
    else v1 = ((IntObject*) obj1)->value;
    if (obj2->parent == float_base) v2 = ((FloatObject*) obj2)->value;
    else v2 = ((IntObject*) obj2)->value;
    return obj_claimed(alloc_bool(context, v1 < v2));
  }
  assert(false);
}

#include <stdio.h>
Object *add(Object *context, Object *fn, Object **args_ptr, int args_len) {
  assert(args_len == 2);
  Object *root = context;
  while (root->parent) root = root->parent;
  Object *int_base = object_lookup(root, "int");
  Object *float_base = object_lookup(root, "float");
  
  Object *obj1 = args_ptr[0], *obj2 = args_ptr[1];
  if (obj1->parent == int_base && obj2->parent == int_base) {
    int i1 = ((IntObject*) obj1)->value, i2 = ((IntObject*) obj2)->value;
    return obj_claimed(alloc_int(context, i1 + i2));
  }
  if ((obj1->parent == float_base || obj1->parent == int_base) && (obj2->parent == float_base || obj2->parent == int_base)) {
    float v1, v2;
    if (obj1->parent == float_base) v1 = ((FloatObject*) obj1)->value;
    else v1 = ((IntObject*) obj1)->value;
    if (obj2->parent == float_base) v2 = ((FloatObject*) obj2)->value;
    else v2 = ((IntObject*) obj2)->value;
    return obj_claimed(alloc_float(context, v1 + v2));
  }
  assert(false);
}

Object *sub(Object *context, Object *fn, Object **args_ptr, int args_len) {
  assert(args_len == 2);
  Object *root = context;
  while (root->parent) root = root->parent;
  Object *int_base = object_lookup(root, "int");
  Object *float_base = object_lookup(root, "float");
  
  Object *obj1 = args_ptr[0], *obj2 = args_ptr[1];
  if (obj1->parent == int_base && obj2->parent == int_base) {
    int i1 = ((IntObject*) obj1)->value, i2 = ((IntObject*) obj2)->value;
    return obj_claimed(alloc_int(context, i1 - i2));
  }
  if ((obj1->parent == float_base || obj1->parent == int_base) && (obj2->parent == float_base || obj2->parent == int_base)) {
    float v1, v2;
    if (obj1->parent == float_base) v1 = ((FloatObject*) obj1)->value;
    else v1 = ((IntObject*) obj1)->value;
    if (obj2->parent == float_base) v2 = ((FloatObject*) obj2)->value;
    else v2 = ((IntObject*) obj2)->value;
    return obj_claimed(alloc_float(context, v1 - v2));
  }
  assert(false);
}

Object *mul(Object *context, Object *fn, Object **args_ptr, int args_len) {
  assert(args_len == 2);
  Object *root = context;
  while (root->parent) root = root->parent;
  Object *int_base = object_lookup(root, "int");
  Object *float_base = object_lookup(root, "float");
  
  Object *obj1 = args_ptr[0], *obj2 = args_ptr[1];
  if (obj1->parent == int_base && obj2->parent == int_base) {
    int i1 = ((IntObject*) obj1)->value, i2 = ((IntObject*) obj2)->value;
    return obj_claimed(alloc_int(context, i1 * i2));
  }
  if ((obj1->parent == float_base || obj1->parent == int_base) && (obj2->parent == float_base || obj2->parent == int_base)) {
    float v1, v2;
    if (obj1->parent == float_base) v1 = ((FloatObject*) obj1)->value;
    else v1 = ((IntObject*) obj1)->value;
    if (obj2->parent == float_base) v2 = ((FloatObject*) obj2)->value;
    else v2 = ((IntObject*) obj2)->value;
    return obj_claimed(alloc_float(context, v1 * v2));
  }
  assert(false);
}

Object *create_root() {
  Object *root = alloc_object(NULL);
  object_set(root, "int", alloc_object(NULL));
  object_set(root, "bool", alloc_object(NULL));
  object_set(root, "float", alloc_object(NULL));
  object_set(root, "function", alloc_object(NULL));
  object_set(root, "=", alloc_fn(root, equals));
  object_set(root, "<", alloc_fn(root, smaller));
  object_set(root, "+", alloc_fn(root, add));
  object_set(root, "-", alloc_fn(root, sub));
  object_set(root, "*", alloc_fn(root, mul));
  return root;
}
