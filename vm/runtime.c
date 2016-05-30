#include "vm/runtime.h"

static Object *equals_fn(Object *context, Object *fn, Object **args_ptr, int args_len) {
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

static Object *smaller_fn(Object *context, Object *fn, Object **args_ptr, int args_len) {
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

static Object *add_fn(Object *context, Object *fn, Object **args_ptr, int args_len) {
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

static Object *sub_fn(Object *context, Object *fn, Object **args_ptr, int args_len) {
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

static Object *mul_fn(Object *context, Object *fn, Object **args_ptr, int args_len) {
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

static Object *div_fn(Object *context, Object *fn, Object **args_ptr, int args_len) {
  assert(args_len == 2);
  Object *root = context;
  while (root->parent) root = root->parent;
  Object *int_base = object_lookup(root, "int");
  Object *float_base = object_lookup(root, "float");
  
  Object *obj1 = args_ptr[0], *obj2 = args_ptr[1];
  if (obj1->parent == int_base && obj2->parent == int_base) {
    int i1 = ((IntObject*) obj1)->value, i2 = ((IntObject*) obj2)->value;
    return obj_claimed(alloc_int(context, i1 / i2));
  }
  if ((obj1->parent == float_base || obj1->parent == int_base) && (obj2->parent == float_base || obj2->parent == int_base)) {
    float v1, v2;
    if (obj1->parent == float_base) v1 = ((FloatObject*) obj1)->value;
    else v1 = ((IntObject*) obj1)->value;
    if (obj2->parent == float_base) v2 = ((FloatObject*) obj2)->value;
    else v2 = ((IntObject*) obj2)->value;
    return obj_claimed(alloc_float(context, v1 / v2));
  }
  assert(false);
}

Object *create_root() {
  Object *root = alloc_object(NULL);
  object_set(root, "int", alloc_object(NULL));
  object_set(root, "bool", alloc_object(NULL));
  object_set(root, "float", alloc_object(NULL));
  object_set(root, "function", alloc_object(NULL));
  object_set(root, "=", alloc_fn(root, equals_fn));
  object_set(root, "<", alloc_fn(root, smaller_fn));
  object_set(root, "+", alloc_fn(root, add_fn));
  object_set(root, "-", alloc_fn(root, sub_fn));
  object_set(root, "*", alloc_fn(root, mul_fn));
  object_set(root, "/", alloc_fn(root, div_fn));
  return root;
}
