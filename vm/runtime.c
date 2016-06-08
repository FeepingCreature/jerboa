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
  
  Object *int_base = object_lookup(root, "int", NULL);
  Object
    *iobj1 = obj_instance_of(thisptr, int_base),
    *iobj2 = obj_instance_of(args_ptr[0], int_base);
  assert(iobj1);
  if (iobj2) {
    int i1 = ((IntObject*) iobj1)->value, i2 = ((IntObject*) iobj2)->value;
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
  
  Object *float_base = object_lookup(root, "float", NULL);
  Object *fobj2 = obj_instance_of(args_ptr[0], float_base);
  if (fobj2) {
    float v1 = ((IntObject*) iobj1)->value, v2 = ((IntObject*) fobj2)->value;
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
  Object *int_base = object_lookup(root, "int", NULL);
  Object *float_base = object_lookup(root, "float", NULL);
  
  Object
    *fobj1 = obj_instance_of(thisptr, float_base),
    *iobj2 = obj_instance_of(args_ptr[0], int_base),
    *fobj2 = obj_instance_of(args_ptr[0], float_base);
  assert(fobj1);
  if (fobj2 || iobj2) {
    float v1 = ((FloatObject*) fobj1)->value, v2;
    if (fobj2) v2 = ((FloatObject*) fobj2)->value;
    else v2 = ((IntObject*) iobj2)->value;
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
  Object *int_base = object_lookup(root, "int", NULL);
  Object *bool_base = object_lookup(root, "bool", NULL);
  Object *float_base = object_lookup(root, "float", NULL);
  Object *string_base = object_lookup(root, "string", NULL);
  
  Object
    *sobj1 = obj_instance_of(thisptr, string_base),
    *iobj2 = obj_instance_of(args_ptr[0], int_base),
    *bobj2 = obj_instance_of(args_ptr[0], bool_base),
    *fobj2 = obj_instance_of(args_ptr[0], float_base),
    *sobj2 = obj_instance_of(args_ptr[0], string_base);
  assert(sobj1);
  
  char *str1 = ((StringObject*) sobj1)->value, *str2;
  if (sobj2) asprintf(&str2, "%s", ((StringObject*) sobj2)->value);
  else if (fobj2) asprintf(&str2, "%f", ((FloatObject*) fobj2)->value);
  else if (iobj2) asprintf(&str2, "%i", ((IntObject*) iobj2)->value);
  else if (bobj2) if (((BoolObject*)bobj2)->value) asprintf(&str2, "%s", "true"); else asprintf(&str2, "%s", "false");
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
  
  Object *int_base = object_lookup(root, "int", NULL);
  Object
    *iobj1 = obj_instance_of(thisptr, int_base),
    *iobj2 = obj_instance_of(args_ptr[0], int_base);
  assert(iobj1);
  if (iobj2) {
    int i1 = ((IntObject*) iobj1)->value, i2 = ((IntObject*) iobj2)->value;
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
  
  Object *float_base = object_lookup(root, "float", NULL);
  Object *fobj2 = obj_instance_of(args_ptr[0], float_base);
  if (fobj2) {
    float v1 = ((IntObject*) iobj1)->value, v2 = ((FloatObject*) fobj2)->value;
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
  
  Object *int_base = object_lookup(root, "int", NULL);
  Object *float_base = object_lookup(root, "float", NULL);
  Object
    *fobj1 = obj_instance_of(thisptr, float_base),
    *iobj2 = obj_instance_of(args_ptr[0], int_base),
    *fobj2 = obj_instance_of(args_ptr[0], float_base);
  assert(fobj1);
  if (fobj2 || iobj2) {
    float v1 = ((FloatObject*) fobj1)->value, v2;
    if (fobj2) v2 = ((FloatObject*) fobj2)->value;
    else v2 = ((IntObject*) iobj2)->value;
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
  Object *root = context;
  while (root->parent) root = root->parent;
  Object *closure_base = object_lookup(root, "closure", NULL);
  ClosureObject *clobj = (ClosureObject*) obj_instance_of(thisptr, closure_base);
  if (clobj) obj_mark(context, clobj->context);
  return NULL;
}

static Object *print_fn(Object *context, Object *thisptr, Object *fn, Object **args_ptr, int args_len) {
  Object *root = context;
  while (root->parent) root = root->parent;
  Object *int_base = object_lookup(root, "int", NULL);
  Object *bool_base = object_lookup(root, "bool", NULL);
  Object *float_base = object_lookup(root, "float", NULL);
  Object *string_base = object_lookup(root, "string", NULL);
  
  for (int i = 0; i < args_len; ++i) {
    Object *arg = args_ptr[i];
    Object
      *iarg = obj_instance_of(arg, int_base),
      *barg = obj_instance_of(arg, bool_base),
      *farg = obj_instance_of(arg, float_base),
      *sarg = obj_instance_of(arg, string_base);
    if (iarg) {
      printf("%i", ((IntObject*)iarg)->value);
      continue;
    }
    if (barg) {
      if (((BoolObject*)barg)->value) printf("true");
      else printf("false");
      continue;
    }
    if (farg) {
      printf("%f", ((FloatObject*)farg)->value);
      continue;
    }
    if (sarg) {
      printf("%s", ((StringObject*)sarg)->value);
      continue;
    }
    assert(false);
  }
  printf("\n");
  return NULL;
}

Object *create_root() {
  Object *root = alloc_object(NULL, NULL);
  
  void *pin_root = gc_add_roots(&root, 1);
  
  object_set(root, "null", NULL);
  
  Object *function_obj = alloc_object(root, NULL);
  function_obj->flags |= OBJ_NOINHERIT;
  object_set(root, "function", function_obj);
  
  Object *closure_obj = alloc_object(root, NULL);
  closure_obj->flags |= OBJ_NOINHERIT;
  object_set(root, "closure", closure_obj);
  object_set(closure_obj, "gc_mark", alloc_fn(root, closure_mark_fn));
  
  Object *bool_obj = alloc_object(root, NULL);
  bool_obj->flags |= OBJ_NOINHERIT;
  object_set(root, "bool", bool_obj);
  object_set(bool_obj, "!", alloc_fn(root, bool_not_fn));
  
  Object *int_obj = alloc_object(root, NULL);
  int_obj->flags |= OBJ_NOINHERIT;
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
  
  Object *float_obj = alloc_object(root, NULL);
  float_obj->flags |= OBJ_NOINHERIT;
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
  
  Object *string_obj = alloc_object(root, NULL);
  string_obj->flags |= OBJ_NOINHERIT;
  object_set(root, "string", string_obj);
  object_set(string_obj, "+", alloc_fn(root, string_add_fn));
  
  object_set(root, "print", alloc_fn(root, print_fn));
  
  gc_remove_roots(pin_root);
  
  return root;
}
