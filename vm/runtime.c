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

static Object *bool_not_fn(Object *context, Object *thisptr, Object *fn, Object **args_ptr, int args_len) {
  assert(args_len == 0);
  Object *root = context;
  while (root->parent) root = root->parent;
  
  return alloc_bool(context, ! ((BoolObject*) thisptr)->value);
}

typedef enum {
  MATH_ADD,
  MATH_SUB,
  MATH_MUL,
  MATH_DIV
} MathOp;

static Object *int_math_fn(Object *context, Object *thisptr, Object *fn, Object **args_ptr, int args_len, MathOp mop) {
  assert(args_len == 1);
  Object *root = context;
  while (root->parent) root = root->parent;
  Object *int_base = object_lookup(root, "int");
  
  Object *obj1 = thisptr, *obj2 = args_ptr[0];
  if (obj2->parent == int_base) {
    int i1 = ((IntObject*) obj1)->value, i2 = ((IntObject*) obj2)->value;
    int res;
    switch (mop) {
      case MATH_ADD: res = i1 + i2; break;
      case MATH_SUB: res = i1 - i2; break;
      case MATH_MUL: res = i1 * i2; break;
      case MATH_DIV: res = i1 / i2; break;
      default: assert(false);
    }
    return alloc_int(context, res);
  }
  
  Object *float_base = object_lookup(root, "float");
  if (obj2->parent == float_base || obj2->parent == int_base) {
    float v1 = ((IntObject*) obj1)->value, v2;
    if (obj2->parent == float_base) v2 = ((FloatObject*) obj2)->value;
    else v2 = ((IntObject*) obj2)->value;
    float res;
    switch (mop) {
      case MATH_ADD: res = v1 + v2; break;
      case MATH_SUB: res = v1 - v2; break;
      case MATH_MUL: res = v1 * v2; break;
      case MATH_DIV: res = v1 / v2; break;
      default: assert(false);
    }
    return alloc_float(context, res);
  }
  assert(false);
}

static Object *int_add_fn(Object *context, Object *thisptr, Object *fn, Object **args_ptr, int args_len) {
  return int_math_fn(context, thisptr, fn, args_ptr, args_len, MATH_ADD);
}

static Object *int_sub_fn(Object *context, Object *thisptr, Object *fn, Object **args_ptr, int args_len) {
  return int_math_fn(context, thisptr, fn, args_ptr, args_len, MATH_SUB);
}

static Object *int_mul_fn(Object *context, Object *thisptr, Object *fn, Object **args_ptr, int args_len) {
  return int_math_fn(context, thisptr, fn, args_ptr, args_len, MATH_MUL);
}

static Object *int_div_fn(Object *context, Object *thisptr, Object *fn, Object **args_ptr, int args_len) {
  return int_math_fn(context, thisptr, fn, args_ptr, args_len, MATH_DIV);
}

static Object *float_math_fn(Object *context, Object *thisptr, Object *fn, Object **args_ptr, int args_len, MathOp mop) {
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
    float res;
    switch (mop) {
      case MATH_ADD: res = v1 + v2; break;
      case MATH_SUB: res = v1 - v2; break;
      case MATH_MUL: res = v1 * v2; break;
      case MATH_DIV: res = v1 / v2; break;
      default: assert(false);
    }
    return alloc_float(context, res);
  }
  assert(false);
}

static Object *float_add_fn(Object *context, Object *thisptr, Object *fn, Object **args_ptr, int args_len) {
  return float_math_fn(context, thisptr, fn, args_ptr, args_len, MATH_ADD);
}

static Object *float_sub_fn(Object *context, Object *thisptr, Object *fn, Object **args_ptr, int args_len) {
  return float_math_fn(context, thisptr, fn, args_ptr, args_len, MATH_SUB);
}

static Object *float_mul_fn(Object *context, Object *thisptr, Object *fn, Object **args_ptr, int args_len) {
  return float_math_fn(context, thisptr, fn, args_ptr, args_len, MATH_MUL);
}

static Object *float_div_fn(Object *context, Object *thisptr, Object *fn, Object **args_ptr, int args_len) {
  return float_math_fn(context, thisptr, fn, args_ptr, args_len, MATH_DIV);
}

static Object *string_add_fn(Object *context, Object *thisptr, Object *fn, Object **args_ptr, int args_len) {
  assert(args_len == 1);
  Object *root = context;
  while (root->parent) root = root->parent;
  Object *int_base = object_lookup(root, "int");
  Object *bool_base = object_lookup(root, "bool");
  Object *float_base = object_lookup(root, "float");
  Object *string_base = object_lookup(root, "string");
  
  Object *obj1 = thisptr, *obj2 = args_ptr[0];
  
  char *str1 = ((StringObject*) obj1)->value, *str2;
  if (obj2->parent == string_base) asprintf(&str2, "%s", ((StringObject*) obj2)->value);
  else if (obj2->parent == float_base) asprintf(&str2, "%f", ((FloatObject*) obj2)->value);
  else if (obj2->parent == int_base) asprintf(&str2, "%i", ((IntObject*) obj2)->value);
  else if (obj2->parent == bool_base) if (((BoolObject*)obj2)->value) asprintf(&str2, "%s", "true"); else asprintf(&str2, "%s", "false");
  else assert(false);
  char *str3;
  asprintf(&str3, "%s%s", str1, str2);
  free(str2);
  Object *res = alloc_string(context, str3);
  free(str3);
  return res;
}

typedef enum {
  CMP_EQ,
  CMP_LT,
  CMP_GT,
  CMP_LE,
  CMP_GE
} CompareOp;

static Object *int_cmp_fn(Object *context, Object *thisptr, Object *fn, Object **args_ptr, int args_len, CompareOp cmp) {
  assert(args_len == 1);
  Object *root = context;
  while (root->parent) root = root->parent;
  Object *int_base = object_lookup(root, "int");
  
  Object *obj1 = thisptr, *obj2 = args_ptr[0];
  if (obj2->parent == int_base) {
    int i1 = ((IntObject*) obj1)->value, i2 = ((IntObject*) obj2)->value;
    bool res;
    switch (cmp) {
      case CMP_EQ: res = i1 == i2; break;
      case CMP_LT: res = i1 <  i2; break;
      case CMP_GT: res = i1 >  i2; break;
      case CMP_LE: res = i1 <= i2; break;
      case CMP_GE: res = i1 >= i2; break;
      default: assert(false);
    }
    return alloc_bool(context, res);
  }
  
  Object *float_base = object_lookup(root, "float");
  if (obj2->parent == float_base || obj2->parent == int_base) {
    float v1 = ((IntObject*) obj1)->value, v2;
    if (obj2->parent == float_base) v2 = ((FloatObject*) obj2)->value;
    else v2 = ((IntObject*) obj2)->value;
    bool res;
    switch (cmp) {
      case CMP_EQ: res = v1 == v2; break;
      case CMP_LT: res = v1 <  v2; break;
      case CMP_GT: res = v1 >  v2; break;
      case CMP_LE: res = v1 <= v2; break;
      case CMP_GE: res = v1 >= v2; break;
      default: assert(false);
    }
    return alloc_bool(context, res);
  }
  assert(false);
}

static Object *int_eq_fn(Object *context, Object *thisptr, Object *fn, Object **args_ptr, int args_len) {
  return int_cmp_fn(context, thisptr, fn, args_ptr, args_len, CMP_EQ);
}

static Object *int_lt_fn(Object *context, Object *thisptr, Object *fn, Object **args_ptr, int args_len) {
  return int_cmp_fn(context, thisptr, fn, args_ptr, args_len, CMP_LT);
}

static Object *int_gt_fn(Object *context, Object *thisptr, Object *fn, Object **args_ptr, int args_len) {
  return int_cmp_fn(context, thisptr, fn, args_ptr, args_len, CMP_GT);
}

static Object *int_le_fn(Object *context, Object *thisptr, Object *fn, Object **args_ptr, int args_len) {
  return int_cmp_fn(context, thisptr, fn, args_ptr, args_len, CMP_LE);
}

static Object *int_ge_fn(Object *context, Object *thisptr, Object *fn, Object **args_ptr, int args_len) {
  return int_cmp_fn(context, thisptr, fn, args_ptr, args_len, CMP_GE);
}

static Object *float_cmp_fn(Object *context, Object *thisptr, Object *fn, Object **args_ptr, int args_len, CompareOp cmp) {
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
    bool res;
    switch (cmp) {
      case CMP_EQ: res = v1 == v2; break;
      case CMP_LT: res = v1 <  v2; break;
      case CMP_GT: res = v1 >  v2; break;
      case CMP_LE: res = v1 <= v2; break;
      case CMP_GE: res = v1 >= v2; break;
      default: assert(false);
    }
    return alloc_bool(context, res);
  }
  assert(false);
}

static Object *float_eq_fn(Object *context, Object *thisptr, Object *fn, Object **args_ptr, int args_len) {
  return float_cmp_fn(context, thisptr, fn, args_ptr, args_len, CMP_EQ);
}

static Object *float_lt_fn(Object *context, Object *thisptr, Object *fn, Object **args_ptr, int args_len) {
  return float_cmp_fn(context, thisptr, fn, args_ptr, args_len, CMP_LT);
}

static Object *float_gt_fn(Object *context, Object *thisptr, Object *fn, Object **args_ptr, int args_len) {
  return float_cmp_fn(context, thisptr, fn, args_ptr, args_len, CMP_GT);
}

static Object *float_le_fn(Object *context, Object *thisptr, Object *fn, Object **args_ptr, int args_len) {
  return float_cmp_fn(context, thisptr, fn, args_ptr, args_len, CMP_LE);
}

static Object *float_ge_fn(Object *context, Object *thisptr, Object *fn, Object **args_ptr, int args_len) {
  return float_cmp_fn(context, thisptr, fn, args_ptr, args_len, CMP_GE);
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
  
  Object *bool_obj = alloc_object(NULL);
  object_set(root, "bool", bool_obj);
  object_set(bool_obj, "!", alloc_fn(root, bool_not_fn));
  
  // same hack
  Object *int_obj = alloc_int(root, 0);
  int_obj->flags &= ~OBJ_CLOSED; // hack hack hack
  object_set(root, "int", int_obj);
  object_set(int_obj, "+" , alloc_fn(root, int_add_fn));
  object_set(int_obj, "-" , alloc_fn(root, int_sub_fn));
  object_set(int_obj, "*" , alloc_fn(root, int_mul_fn));
  object_set(int_obj, "/" , alloc_fn(root, int_div_fn));
  object_set(int_obj, "==", alloc_fn(root, int_eq_fn));
  object_set(int_obj, "<" , alloc_fn(root, int_lt_fn));
  object_set(int_obj, ">" , alloc_fn(root, int_gt_fn));
  object_set(int_obj, "<=", alloc_fn(root, int_le_fn));
  object_set(int_obj, ">=", alloc_fn(root, int_ge_fn));
  
  // same hack
  Object *float_obj = alloc_float(root, 0);
  float_obj->flags &= ~OBJ_CLOSED; // hack hack hack
  object_set(root, "float", float_obj);
  object_set(float_obj, "+" , alloc_fn(root, float_add_fn));
  object_set(float_obj, "-" , alloc_fn(root, float_sub_fn));
  object_set(float_obj, "*" , alloc_fn(root, float_mul_fn));
  object_set(float_obj, "/" , alloc_fn(root, float_div_fn));
  object_set(float_obj, "==", alloc_fn(root, float_eq_fn));
  object_set(float_obj, "<" , alloc_fn(root, float_lt_fn));
  object_set(float_obj, ">" , alloc_fn(root, float_gt_fn));
  object_set(float_obj, "<=", alloc_fn(root, float_le_fn));
  object_set(float_obj, ">=", alloc_fn(root, float_ge_fn));
  
  Object *string_obj = alloc_object(NULL);
  object_set(root, "string", string_obj);
  object_set(string_obj, "+", alloc_fn(root, string_add_fn));
  
  object_set(root, "print", alloc_fn(root, print_fn));
  
  gc_remove_roots(pin_root);
  
  return root;
}
