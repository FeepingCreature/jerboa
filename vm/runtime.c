#include "vm/runtime.h"

static Object *equals_fn(Object *context, Object *thisptr, Object *fn, Object **args_ptr, int args_len) {
  assert(args_len == 2);
  Object *root = context;
  while (root->parent) root = root->parent;
  Object *int_base = object_lookup(root, "int");
  Object *float_base = object_lookup(root, "float");
  
  Object *obj1 = args_ptr[0], *obj2 = args_ptr[1];
  if (obj1->parent == int_base && obj2->parent == int_base) {
    int test = ((IntObject*) obj1)->value == ((IntObject*) obj2)->value;
    return alloc_bool(context, test?true:false);
  }
  if ((obj1->parent == float_base || obj1->parent == int_base) && (obj2->parent == float_base || obj2->parent == int_base)) {
    float v1, v2;
    if (obj1->parent == float_base) v1 = ((FloatObject*) obj1)->value;
    else v1 = ((IntObject*) obj1)->value;
    if (obj2->parent == float_base) v2 = ((FloatObject*) obj2)->value;
    else v2 = ((IntObject*) obj2)->value;
    return alloc_bool(context, v1 == v2);
  }
  assert(false);
}

static Object *smaller_fn(Object *context, Object *thisptr, Object *fn, Object **args_ptr, int args_len) {
  assert(args_len == 2);
  Object *root = context;
  while (root->parent) root = root->parent;
  Object *int_base = object_lookup(root, "int");
  Object *float_base = object_lookup(root, "float");
  
  Object *obj1 = args_ptr[0], *obj2 = args_ptr[1];
  if (obj1->parent == int_base && obj2->parent == int_base) {
    int i1 = ((IntObject*) obj1)->value, i2 = ((IntObject*) obj2)->value;
    return alloc_bool(context, i1 < i2);
  }
  if ((obj1->parent == float_base || obj1->parent == int_base) && (obj2->parent == float_base || obj2->parent == int_base)) {
    float v1, v2;
    if (obj1->parent == float_base) v1 = ((FloatObject*) obj1)->value;
    else v1 = ((IntObject*) obj1)->value;
    if (obj2->parent == float_base) v2 = ((FloatObject*) obj2)->value;
    else v2 = ((IntObject*) obj2)->value;
    return alloc_bool(context, v1 < v2);
  }
  assert(false);
}

static Object *add_fn(Object *context, Object *thisptr, Object *fn, Object **args_ptr, int args_len) {
  assert(args_len == 2);
  Object *root = context;
  while (root->parent) root = root->parent;
  Object *int_base = object_lookup(root, "int");
  Object *float_base = object_lookup(root, "float");
  
  Object *obj1 = args_ptr[0], *obj2 = args_ptr[1];
  if (obj1->parent == int_base && obj2->parent == int_base) {
    int i1 = ((IntObject*) obj1)->value, i2 = ((IntObject*) obj2)->value;
    return alloc_int(context, i1 + i2);
  }
  if ((obj1->parent == float_base || obj1->parent == int_base) && (obj2->parent == float_base || obj2->parent == int_base)) {
    float v1, v2;
    if (obj1->parent == float_base) v1 = ((FloatObject*) obj1)->value;
    else v1 = ((IntObject*) obj1)->value;
    if (obj2->parent == float_base) v2 = ((FloatObject*) obj2)->value;
    else v2 = ((IntObject*) obj2)->value;
    return alloc_float(context, v1 + v2);
  }
  assert(false);
}

static Object *sub_fn(Object *context, Object *thisptr, Object *fn, Object **args_ptr, int args_len) {
  assert(args_len == 2);
  Object *root = context;
  while (root->parent) root = root->parent;
  Object *int_base = object_lookup(root, "int");
  Object *float_base = object_lookup(root, "float");
  
  Object *obj1 = args_ptr[0], *obj2 = args_ptr[1];
  if (obj1->parent == int_base && obj2->parent == int_base) {
    int i1 = ((IntObject*) obj1)->value, i2 = ((IntObject*) obj2)->value;
    return alloc_int(context, i1 - i2);
  }
  if ((obj1->parent == float_base || obj1->parent == int_base) && (obj2->parent == float_base || obj2->parent == int_base)) {
    float v1, v2;
    if (obj1->parent == float_base) v1 = ((FloatObject*) obj1)->value;
    else v1 = ((IntObject*) obj1)->value;
    if (obj2->parent == float_base) v2 = ((FloatObject*) obj2)->value;
    else v2 = ((IntObject*) obj2)->value;
    return alloc_float(context, v1 - v2);
  }
  assert(false);
}

static Object *mul_fn(Object *context, Object *thisptr, Object *fn, Object **args_ptr, int args_len) {
  assert(args_len == 2);
  Object *root = context;
  while (root->parent) root = root->parent;
  Object *int_base = object_lookup(root, "int");
  Object *float_base = object_lookup(root, "float");
  
  Object *obj1 = args_ptr[0], *obj2 = args_ptr[1];
  if (obj1->parent == int_base && obj2->parent == int_base) {
    int i1 = ((IntObject*) obj1)->value, i2 = ((IntObject*) obj2)->value;
    return alloc_int(context, i1 * i2);
  }
  if ((obj1->parent == float_base || obj1->parent == int_base) && (obj2->parent == float_base || obj2->parent == int_base)) {
    float v1, v2;
    if (obj1->parent == float_base) v1 = ((FloatObject*) obj1)->value;
    else v1 = ((IntObject*) obj1)->value;
    if (obj2->parent == float_base) v2 = ((FloatObject*) obj2)->value;
    else v2 = ((IntObject*) obj2)->value;
    return alloc_float(context, v1 * v2);
  }
  assert(false);
}

static Object *div_fn(Object *context, Object *thisptr, Object *fn, Object **args_ptr, int args_len) {
  assert(args_len == 2);
  Object *root = context;
  while (root->parent) root = root->parent;
  Object *int_base = object_lookup(root, "int");
  Object *float_base = object_lookup(root, "float");
  
  Object *obj1 = args_ptr[0], *obj2 = args_ptr[1];
  if (obj1->parent == int_base && obj2->parent == int_base) {
    int i1 = ((IntObject*) obj1)->value, i2 = ((IntObject*) obj2)->value;
    return alloc_int(context, i1 / i2);
  }
  if ((obj1->parent == float_base || obj1->parent == int_base) && (obj2->parent == float_base || obj2->parent == int_base)) {
    float v1, v2;
    if (obj1->parent == float_base) v1 = ((FloatObject*) obj1)->value;
    else v1 = ((IntObject*) obj1)->value;
    if (obj2->parent == float_base) v2 = ((FloatObject*) obj2)->value;
    else v2 = ((IntObject*) obj2)->value;
    return alloc_float(context, v1 / v2);
  }
  assert(false);
}

static Object *closure_mark_fn(Object *context, Object *thisptr, Object *fn, Object **args_ptr, int args_len) {
  ClosureObject *clobj = (ClosureObject*) thisptr;
  obj_mark(clobj->context);
  return NULL;
}

#include <stdio.h>
static Object *print_fn(Object *context, Object *thisptr, Object *fn, Object **args_ptr, int args_len) {
  Object *root = context;
  while (root->parent) root = root->parent;
  Object *int_base = object_lookup(root, "int");
  Object *float_base = object_lookup(root, "float");
  Object *string_base = object_lookup(root, "string");
  
  for (int i = 0; i < args_len; ++i) {
    Object *arg = args_ptr[i];
    if (arg->parent == int_base) {
      printf("%i", ((IntObject*)arg)->value);
      continue;
    }
    if (arg->parent == float_base) {
      printf("%f", ((FloatObject*)arg)->value);
      continue;
    }
    if (arg->parent == string_base) {
      printf("%s", ((StringObject*)arg)->value);
      continue;
    }
    assert(false);
  }
  printf("\n");
  return NULL;
}

Object *create_root() {
  Object *root = alloc_object(NULL);
  object_set(root, "int", alloc_object(NULL));
  object_set(root, "bool", alloc_object(NULL));
  object_set(root, "float", alloc_object(NULL));
  object_set(root, "string", alloc_object(NULL));
  Object *function_obj = alloc_object(NULL);
  Object *closure_obj = alloc_object(function_obj);
  object_set(closure_obj, "gc_mark", alloc_fn(root, closure_mark_fn));
  object_set(root, "function", function_obj);
  object_set(root, "closure", closure_obj);
  object_set(root, "=", alloc_fn(root, equals_fn));
  object_set(root, "<", alloc_fn(root, smaller_fn));
  object_set(root, "+", alloc_fn(root, add_fn));
  object_set(root, "-", alloc_fn(root, sub_fn));
  object_set(root, "*", alloc_fn(root, mul_fn));
  object_set(root, "/", alloc_fn(root, div_fn));
  object_set(root, "print", alloc_fn(root, print_fn));
  return root;
}
