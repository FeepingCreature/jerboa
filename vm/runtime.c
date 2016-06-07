#include "vm/runtime.h"

#include <stdio.h>
#include <stdarg.h>

#include "vm/call.h"
#include "gc.h"

static void asprintf(char **outp, char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int len = vsnprintf(NULL, 0, fmt, ap);
  *outp = malloc(len + 1);
  va_end(ap);
  va_start(ap, fmt);
  vsnprintf(*outp, len + 1, fmt, ap);
  va_end(ap);
}

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

static Object *int_add_fn(Object *context, Object *thisptr, Object *fn, Object **args_ptr, int args_len) {
  assert(args_len == 1);
  Object *root = context;
  while (root->parent) root = root->parent;
  Object *int_base = object_lookup(root, "int");
  
  Object *obj1 = thisptr, *obj2 = args_ptr[0];
  if (obj2->parent == int_base) {
    int i1 = ((IntObject*) obj1)->value, i2 = ((IntObject*) obj2)->value;
    return alloc_int(context, i1 + i2);
  }
  
  Object *float_base = object_lookup(root, "float");
  if (obj2->parent == float_base || obj2->parent == int_base) {
    float v1 = ((IntObject*) obj1)->value, v2;
    if (obj2->parent == float_base) v2 = ((FloatObject*) obj2)->value;
    else v2 = ((IntObject*) obj2)->value;
    return alloc_float(context, v1 + v2);
  }
  assert(false);
}

static Object *float_add_fn(Object *context, Object *thisptr, Object *fn, Object **args_ptr, int args_len) {
  assert(args_len == 1);
  Object *root = context;
  while (root->parent) root = root->parent;
  Object *int_base = object_lookup(root, "int");
  Object *float_base = object_lookup(root, "float");
  
  Object *obj1 = thisptr, *obj2 = args_ptr[0];
  if (obj2->parent == float_base || obj2->parent == int_base) {
    float v1 = ((FloatObject*) obj1)->value, v2;
    if (obj2->parent == float_base) v2 = ((FloatObject*) obj2)->value;
    else v2 = ((IntObject*) obj2)->value;
    return alloc_float(context, v1 + v2);
  }
  assert(false);
}

static Object *string_add_fn(Object *context, Object *thisptr, Object *fn, Object **args_ptr, int args_len) {
  assert(args_len == 1);
  Object *root = context;
  while (root->parent) root = root->parent;
  Object *int_base = object_lookup(root, "int");
  Object *float_base = object_lookup(root, "float");
  Object *string_base = object_lookup(root, "string");
  
  Object *obj1 = thisptr, *obj2 = args_ptr[0];
  
  char *str1 = ((StringObject*) obj1)->value, *str2;
  if (obj2->parent == string_base) asprintf(&str2, "%s", ((StringObject*) obj2)->value);
  else if (obj2->parent == float_base) asprintf(&str2, "%f", ((FloatObject*) obj2)->value);
  else if (obj2->parent == int_base) asprintf(&str2, "%i", ((IntObject*) obj2)->value);
  else assert(false);
  char *str3;
  asprintf(&str3, "%s%s", str1, str2);
  free(str2);
  Object *res = alloc_string(context, str3);
  free(str3);
  return res;
}

static Object *int_sub_fn(Object *context, Object *thisptr, Object *fn, Object **args_ptr, int args_len) {
  assert(args_len == 1);
  Object *root = context;
  while (root->parent) root = root->parent;
  Object *int_base = object_lookup(root, "int");
  
  Object *obj1 = thisptr, *obj2 = args_ptr[0];
  if (obj2->parent == int_base) {
    int i1 = ((IntObject*) obj1)->value, i2 = ((IntObject*) obj2)->value;
    return alloc_int(context, i1 - i2);
  }
  
  Object *float_base = object_lookup(root, "float");
  if (obj2->parent == float_base || obj2->parent == int_base) {
    float v1 = ((IntObject*) obj1)->value, v2;
    if (obj2->parent == float_base) v2 = ((FloatObject*) obj2)->value;
    else v2 = ((IntObject*) obj2)->value;
    return alloc_float(context, v1 - v2);
  }
  assert(false);
}

static Object *float_sub_fn(Object *context, Object *thisptr, Object *fn, Object **args_ptr, int args_len) {
  assert(args_len == 1);
  Object *root = context;
  while (root->parent) root = root->parent;
  Object *int_base = object_lookup(root, "int");
  Object *float_base = object_lookup(root, "float");
  
  Object *obj1 = thisptr, *obj2 = args_ptr[0];
  if (obj2->parent == float_base || obj2->parent == int_base) {
    float v1 = ((FloatObject*) obj1)->value, v2;
    if (obj2->parent == float_base) v2 = ((FloatObject*) obj2)->value;
    else v2 = ((IntObject*) obj2)->value;
    return alloc_float(context, v1 - v2);
  }
  assert(false);
}

static Object *int_mul_fn(Object *context, Object *thisptr, Object *fn, Object **args_ptr, int args_len) {
  assert(args_len == 1);
  Object *root = context;
  while (root->parent) root = root->parent;
  Object *int_base = object_lookup(root, "int");
  
  Object *obj1 = thisptr, *obj2 = args_ptr[0];
  if (obj2->parent == int_base) {
    int i1 = ((IntObject*) obj1)->value, i2 = ((IntObject*) obj2)->value;
    return alloc_int(context, i1 * i2);
  }
  
  Object *float_base = object_lookup(root, "float");
  if (obj2->parent == float_base || obj2->parent == int_base) {
    float v1 = ((IntObject*) obj1)->value, v2;
    if (obj2->parent == float_base) v2 = ((FloatObject*) obj2)->value;
    else v2 = ((IntObject*) obj2)->value;
    return alloc_float(context, v1 * v2);
  }
  assert(false);
}

static Object *float_mul_fn(Object *context, Object *thisptr, Object *fn, Object **args_ptr, int args_len) {
  assert(args_len == 1);
  Object *root = context;
  while (root->parent) root = root->parent;
  Object *int_base = object_lookup(root, "int");
  Object *float_base = object_lookup(root, "float");
  
  Object *obj1 = thisptr, *obj2 = args_ptr[0];
  if (obj2->parent == float_base || obj2->parent == int_base) {
    float v1 = ((FloatObject*) obj1)->value, v2;
    if (obj2->parent == float_base) v2 = ((FloatObject*) obj2)->value;
    else v2 = ((IntObject*) obj2)->value;
    return alloc_float(context, v1 * v2);
  }
  assert(false);
}

static Object *int_div_fn(Object *context, Object *thisptr, Object *fn, Object **args_ptr, int args_len) {
  assert(args_len == 1);
  Object *root = context;
  while (root->parent) root = root->parent;
  Object *int_base = object_lookup(root, "int");
  
  Object *obj1 = thisptr, *obj2 = args_ptr[0];
  if (obj2->parent == int_base) {
    int i1 = ((IntObject*) obj1)->value, i2 = ((IntObject*) obj2)->value;
    return alloc_int(context, i1 / i2);
  }
  
  Object *float_base = object_lookup(root, "float");
  if (obj2->parent == float_base || obj2->parent == int_base) {
    float v1 = ((IntObject*) obj1)->value, v2;
    if (obj2->parent == float_base) v2 = ((FloatObject*) obj2)->value;
    else v2 = ((IntObject*) obj2)->value;
    return alloc_float(context, v1 / v2);
  }
  assert(false);
}

static Object *float_div_fn(Object *context, Object *thisptr, Object *fn, Object **args_ptr, int args_len) {
  assert(args_len == 1);
  Object *root = context;
  while (root->parent) root = root->parent;
  Object *int_base = object_lookup(root, "int");
  Object *float_base = object_lookup(root, "float");
  
  Object *obj1 = thisptr, *obj2 = args_ptr[0];
  if (obj2->parent == float_base || obj2->parent == int_base) {
    float v1 = ((FloatObject*) obj1)->value, v2;
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
  
  void *pin_root = gc_add_roots(&root, 1);
  
  object_set(root, "bool", alloc_object(NULL));
  Object *function_obj = alloc_object(NULL);
  object_set(root, "function", function_obj);
  
  // terrible hack. see, "closure" is the prototype for all closures, so it contains a gc_mark function
  // so it has to be an actual closure itself (albeit with a null context/function pointer)
  UserFunction bogus;
  Object *closure_obj = alloc_closure_fn(root, &bogus);
  ((ClosureObject*) closure_obj)->context = NULL;
  ((ClosureObject*) closure_obj)->base.fn_ptr = NULL;
  object_set(root, "closure", closure_obj);
  object_set(closure_obj, "gc_mark", alloc_fn(root, closure_mark_fn));
  
  Object *int_obj = alloc_object(NULL);
  object_set(root, "int", int_obj);
  object_set(int_obj, "+", alloc_fn(root, int_add_fn));
  object_set(int_obj, "-", alloc_fn(root, int_sub_fn));
  object_set(int_obj, "*", alloc_fn(root, int_mul_fn));
  object_set(int_obj, "/", alloc_fn(root, int_div_fn));
  
  Object *float_obj = alloc_object(NULL);
  object_set(root, "float", float_obj);
  object_set(float_obj, "+", alloc_fn(root, float_add_fn));
  object_set(float_obj, "-", alloc_fn(root, float_sub_fn));
  object_set(float_obj, "*", alloc_fn(root, float_mul_fn));
  object_set(float_obj, "/", alloc_fn(root, float_div_fn));
  
  Object *string_obj = alloc_object(NULL);
  object_set(root, "string", string_obj);
  object_set(string_obj, "+", alloc_fn(root, string_add_fn));
  
  object_set(root, "=", alloc_fn(root, equals_fn));
  object_set(root, "<", alloc_fn(root, smaller_fn));
  object_set(root, "print", alloc_fn(root, print_fn));
  
  gc_remove_roots(pin_root);
  
  return root;
}
