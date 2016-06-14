#include "vm/runtime.h"

#include <stdio.h>
#include <stdarg.h>
#include <alloca.h>

#include <ffi.h>
#include <dlfcn.h>

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

static void bool_not_fn(VMState *state, Object *thisptr, Object *fn, Object **args_ptr, int args_len) {
  assert(args_len == 0);
  Object *root = state->stack_ptr[state->stack_len - 1].context;
  while (root->parent) root = root->parent;
  
  state->result_value = alloc_bool(state, ! ((BoolObject*) thisptr)->value);
}

typedef enum {
  MATH_ADD,
  MATH_SUB,
  MATH_MUL,
  MATH_DIV
} MathOp;

static void int_math_fn(VMState *state, Object *thisptr, Object *fn, Object **args_ptr, int args_len, MathOp mop) {
  assert(args_len == 1);
  Object *root = state->stack_ptr[state->stack_len - 1].context;
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
    state->result_value = alloc_int(state, res);
    return;
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
    state->result_value = alloc_float(state, res);
    return;
  }
  assert(false);
}

static void int_add_fn(VMState *state, Object *thisptr, Object *fn, Object **args_ptr, int args_len) {
  int_math_fn(state, thisptr, fn, args_ptr, args_len, MATH_ADD);
}

static void int_sub_fn(VMState *state, Object *thisptr, Object *fn, Object **args_ptr, int args_len) {
  int_math_fn(state, thisptr, fn, args_ptr, args_len, MATH_SUB);
}

static void int_mul_fn(VMState *state, Object *thisptr, Object *fn, Object **args_ptr, int args_len) {
  int_math_fn(state, thisptr, fn, args_ptr, args_len, MATH_MUL);
}

static void int_div_fn(VMState *state, Object *thisptr, Object *fn, Object **args_ptr, int args_len) {
  int_math_fn(state, thisptr, fn, args_ptr, args_len, MATH_DIV);
}

static void float_math_fn(VMState *state, Object *thisptr, Object *fn, Object **args_ptr, int args_len, MathOp mop) {
  assert(args_len == 1);
  Object *root = state->stack_ptr[state->stack_len - 1].context;
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
    state->result_value = alloc_float(state, res);
    return;
  }
  assert(false);
}

static void float_add_fn(VMState *state, Object *thisptr, Object *fn, Object **args_ptr, int args_len) {
  float_math_fn(state, thisptr, fn, args_ptr, args_len, MATH_ADD);
}

static void float_sub_fn(VMState *state, Object *thisptr, Object *fn, Object **args_ptr, int args_len) {
  float_math_fn(state, thisptr, fn, args_ptr, args_len, MATH_SUB);
}

static void float_mul_fn(VMState *state, Object *thisptr, Object *fn, Object **args_ptr, int args_len) {
  float_math_fn(state, thisptr, fn, args_ptr, args_len, MATH_MUL);
}

static void float_div_fn(VMState *state, Object *thisptr, Object *fn, Object **args_ptr, int args_len) {
  float_math_fn(state, thisptr, fn, args_ptr, args_len, MATH_DIV);
}

static void string_add_fn(VMState *state, Object *thisptr, Object *fn, Object **args_ptr, int args_len) {
  assert(args_len == 1);
  Object *root = state->stack_ptr[state->stack_len - 1].context;
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
  state->result_value = alloc_string(state, str3);
  free(str3);
}

typedef enum {
  CMP_EQ,
  CMP_LT,
  CMP_GT,
  CMP_LE,
  CMP_GE
} CompareOp;

static void int_cmp_fn(VMState *state, Object *thisptr, Object *fn, Object **args_ptr, int args_len, CompareOp cmp) {
  assert(args_len == 1);
  Object *root = state->stack_ptr[state->stack_len - 1].context;
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
    state->result_value = alloc_bool(state, res);
    return;
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
    state->result_value = alloc_bool(state, res);
    return;
  }
  assert(false);
}

static void int_eq_fn(VMState *state, Object *thisptr, Object *fn, Object **args_ptr, int args_len) {
  int_cmp_fn(state, thisptr, fn, args_ptr, args_len, CMP_EQ);
}

static void int_lt_fn(VMState *state, Object *thisptr, Object *fn, Object **args_ptr, int args_len) {
  int_cmp_fn(state, thisptr, fn, args_ptr, args_len, CMP_LT);
}

static void int_gt_fn(VMState *state, Object *thisptr, Object *fn, Object **args_ptr, int args_len) {
  int_cmp_fn(state, thisptr, fn, args_ptr, args_len, CMP_GT);
}

static void int_le_fn(VMState *state, Object *thisptr, Object *fn, Object **args_ptr, int args_len) {
  int_cmp_fn(state, thisptr, fn, args_ptr, args_len, CMP_LE);
}

static void int_ge_fn(VMState *state, Object *thisptr, Object *fn, Object **args_ptr, int args_len) {
  int_cmp_fn(state, thisptr, fn, args_ptr, args_len, CMP_GE);
}

static void float_cmp_fn(VMState *state, Object *thisptr, Object *fn, Object **args_ptr, int args_len, CompareOp cmp) {
  assert(args_len == 1);
  Object *root = state->stack_ptr[state->stack_len - 1].context;
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
    state->result_value = alloc_bool(state, res);
    return;
  }
  assert(false);
}

static void float_eq_fn(VMState *state, Object *thisptr, Object *fn, Object **args_ptr, int args_len) {
  float_cmp_fn(state, thisptr, fn, args_ptr, args_len, CMP_EQ);
}

static void float_lt_fn(VMState *state, Object *thisptr, Object *fn, Object **args_ptr, int args_len) {
  float_cmp_fn(state, thisptr, fn, args_ptr, args_len, CMP_LT);
}

static void float_gt_fn(VMState *state, Object *thisptr, Object *fn, Object **args_ptr, int args_len) {
  float_cmp_fn(state, thisptr, fn, args_ptr, args_len, CMP_GT);
}

static void float_le_fn(VMState *state, Object *thisptr, Object *fn, Object **args_ptr, int args_len) {
  float_cmp_fn(state, thisptr, fn, args_ptr, args_len, CMP_LE);
}

static void float_ge_fn(VMState *state, Object *thisptr, Object *fn, Object **args_ptr, int args_len) {
  float_cmp_fn(state, thisptr, fn, args_ptr, args_len, CMP_GE);
}

static void closure_mark_fn(VMState *state, Object *obj) {
  Object *root = state->stack_ptr[state->stack_len - 1].context;
  while (root->parent) root = root->parent;
  Object *closure_base = object_lookup(root, "closure", NULL);
  ClosureObject *clobj = (ClosureObject*) obj_instance_of(obj, closure_base);
  if (clobj) obj_mark(state, clobj->context);
}

static void array_mark_fn(VMState *state, Object *obj) {
  Object *root = state->stack_ptr[state->stack_len - 1].context;
  while (root->parent) root = root->parent;
  Object *array_base = object_lookup(root, "array", NULL);
  ArrayObject *arr_obj = (ArrayObject*) obj_instance_of(obj, array_base);
  if (arr_obj) {
    for (int i = 0; i < arr_obj->length; ++i) {
      obj_mark(state, arr_obj->ptr[i]);
    }
  }
}

static void array_resize_fn(VMState *state, Object *thisptr, Object *fn, Object **args_ptr, int args_len) {
  assert(args_len == 1);
  Object *root = state->stack_ptr[state->stack_len - 1].context;
  while (root->parent) root = root->parent;
  Object *int_base = object_lookup(root, "int", NULL);
  Object *array_base = object_lookup(root, "array", NULL);
  ArrayObject *arr_obj = (ArrayObject*) obj_instance_of(thisptr, array_base);
  IntObject *iarg = (IntObject*) obj_instance_of(args_ptr[0], int_base);
  assert(iarg);
  assert(arr_obj);
  int newsize = iarg->value;
  assert(newsize >= 0);
  arr_obj->ptr = realloc(arr_obj->ptr, sizeof(Object*) * newsize);
  arr_obj->length = newsize;
  object_set(thisptr, "length", alloc_int(state, newsize));
  state->result_value = thisptr;
}

static void array_push_fn(VMState *state, Object *thisptr, Object *fn, Object **args_ptr, int args_len) {
  assert(args_len == 1);
  Object *root = state->stack_ptr[state->stack_len - 1].context;
  while (root->parent) root = root->parent;
  Object *array_base = object_lookup(root, "array", NULL);
  ArrayObject *arr_obj = (ArrayObject*) obj_instance_of(thisptr, array_base);
  assert(arr_obj);
  Object *value = args_ptr[0];
  arr_obj->ptr = realloc(arr_obj->ptr, sizeof(Object*) * ++arr_obj->length);
  arr_obj->ptr[arr_obj->length - 1] = value;
  object_set(thisptr, "length", alloc_int(state, arr_obj->length));
  state->result_value = thisptr;
}

static void array_pop_fn(VMState *state, Object *thisptr, Object *fn, Object **args_ptr, int args_len) {
  assert(args_len == 0);
  Object *root = state->stack_ptr[state->stack_len - 1].context;
  while (root->parent) root = root->parent;
  Object *array_base = object_lookup(root, "array", NULL);
  ArrayObject *arr_obj = (ArrayObject*) obj_instance_of(thisptr, array_base);
  assert(arr_obj);
  Object *res = arr_obj->ptr[arr_obj->length - 1];
  arr_obj->ptr = realloc(arr_obj->ptr, sizeof(Object*) * --arr_obj->length);
  object_set(thisptr, "length", alloc_int(state, arr_obj->length));
  state->result_value = res;
}

static void array_index_fn(VMState *state, Object *thisptr, Object *fn, Object **args_ptr, int args_len) {
  assert(args_len == 1);
  Object *root = state->stack_ptr[state->stack_len - 1].context;
  while (root->parent) root = root->parent;
  Object *int_base = object_lookup(root, "int", NULL);
  Object *array_base = object_lookup(root, "array", NULL);
  ArrayObject *arr_obj = (ArrayObject*) obj_instance_of(thisptr, array_base);
  IntObject *iarg = (IntObject*) obj_instance_of(args_ptr[0], int_base);
  if (!iarg) { state->result_value = NULL; return; }
  assert(arr_obj);
  int index = iarg->value;
  assert(index >= 0 && index < arr_obj->length);
  state->result_value = arr_obj->ptr[index];
}

static void array_index_assign_fn(VMState *state, Object *thisptr, Object *fn, Object **args_ptr, int args_len) {
  assert(args_len == 2);
  Object *root = state->stack_ptr[state->stack_len - 1].context;
  while (root->parent) root = root->parent;
  Object *int_base = object_lookup(root, "int", NULL);
  Object *array_base = object_lookup(root, "array", NULL);
  ArrayObject *arr_obj = (ArrayObject*) obj_instance_of(thisptr, array_base);
  IntObject *iarg = (IntObject*) obj_instance_of(args_ptr[0], int_base);
  assert(arr_obj);
  assert(iarg);
  int index = iarg->value;
  assert(index >= 0 && index < arr_obj->length);
  Object *value = args_ptr[1];
  arr_obj->ptr[index] = value;
  state->result_value = NULL;
}

static void print_fn(VMState *state, Object *thisptr, Object *fn, Object **args_ptr, int args_len) {
  Object *root = state->stack_ptr[state->stack_len - 1].context;
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
  state->result_value = NULL;
}

static void ffi_open_fn(VMState *state, Object *thisptr, Object *fn, Object **args_ptr, int args_len) {
  Object *root = state->stack_ptr[state->stack_len - 1].context;
  while (root->parent) root = root->parent;
  Object *string_base = object_lookup(root, "string", NULL);
  Object *ffi = object_lookup(root, "ffi", NULL);
  Object *handle_base = object_lookup(ffi, "handle", NULL);
  
  assert(args_len == 1);
  StringObject *sarg = (StringObject*) obj_instance_of(args_ptr[0], string_base);
  
  char *file = sarg->value;
  void *dlptr = dlopen(file, RTLD_LAZY);
  if (dlptr == NULL) {
    fprintf(stderr, "dlopen failed: %s\n", dlerror());
    assert(false);
  }
  
  Object *handle_obj = alloc_object(state, handle_base);
  handle_obj->flags |= OBJ_IMMUTABLE;
  object_set(handle_obj, "pointer", alloc_ptr(state, dlptr));
  state->result_value = handle_obj;
}

typedef struct {
  ffi_cif cif;
} FFIHandle;

static ffi_type *type_to_ffi_ptr(Object *ffi, Object *obj) {
  Object *sint = object_lookup(ffi, "sint", NULL);
  Object *pointer = object_lookup(ffi, "pointer", NULL);
  if (obj_instance_of(obj, sint)) return &ffi_type_sint;
  if (obj_instance_of(obj, pointer)) return &ffi_type_pointer;
  assert("Unknown type." && false);
}

static void ffi_call_fn(VMState *state, Object *thisptr, Object *fn, Object **args_ptr, int args_len) {
  Object *root = state->stack_ptr[state->stack_len - 1].context;
  while (root->parent) root = root->parent;
  Object *int_base = object_lookup(root, "int", NULL);
  Object *array_base = object_lookup(root, "array", NULL);
  Object *string_base = object_lookup(root, "string", NULL);
  Object *pointer_base = object_lookup(root, "pointer", NULL);
  
  Object *ffi = object_lookup(root, "ffi", NULL);
  Object *ffi_sint = object_lookup(ffi, "sint", NULL);
  Object *ffi_ptr = object_lookup(ffi, "ptr", NULL);
  Object *ffi_charptr = object_lookup(ffi, "char_ptr", NULL);
  
  Object *ret_type = object_lookup(fn, "return_type", NULL);
  Object *par_types = object_lookup(fn, "parameter_types", NULL);
  Object *ffi_ptr_obj = object_lookup(fn, "_ffi_pointer", NULL);
  Object *sym_ptr_obj = object_lookup(fn, "_sym_pointer", NULL);
  assert(ret_type && par_types && ffi_ptr_obj && sym_ptr_obj);
  ArrayObject *par_types_array = (ArrayObject*) obj_instance_of(par_types, array_base);
  assert(par_types_array);
  PointerObject *ffi_ptr_obj_sub = (PointerObject*) obj_instance_of(ffi_ptr_obj, pointer_base);
  PointerObject *sym_ptr_obj_sub = (PointerObject*) obj_instance_of(sym_ptr_obj, pointer_base);
  assert(ffi_ptr_obj_sub && sym_ptr_obj_sub);
  FFIHandle *ffihdl = ffi_ptr_obj_sub->ptr;
  void *sym_ptr = sym_ptr_obj_sub->ptr;
  
  if (args_len != par_types_array->length) {
    fprintf(stderr, "FFI arity violated: expected %i, got %i\n", par_types_array->length, args_len);
    assert(false);
  }
  
  int par_len_sum = 0;
  for (int i = -1; i < args_len; ++i) {
    Object *type;
    if (i == -1) type = ret_type;
    else type = par_types_array->ptr[i];
    
    if (obj_instance_of_or_equal(type, ffi_sint)) par_len_sum += sizeof(int);
    else if (obj_instance_of_or_equal(type, ffi_ptr)) par_len_sum += sizeof(void*);
    else assert(false);
  }
  
  void *ret_ptr;
  void **par_ptrs = alloca(sizeof(void*) * args_len);
  void *data = alloca(par_len_sum);
  for (int i = -1; i < args_len; ++i) {
    Object *type;
    if (i == -1) type = ret_type;
    else type = par_types_array->ptr[i];
    
    if (obj_instance_of_or_equal(type, ffi_sint)) {
      if (i == -1) {
        ret_ptr = data;
      } else {
        IntObject *iobj = (IntObject*) obj_instance_of(args_ptr[i], int_base);
        assert(iobj);
        *(int*) data = iobj->value;
        par_ptrs[i] = data;
      }
      
      data = (char*) data + sizeof(long);
    }
    else if (obj_instance_of_or_equal(type, ffi_charptr)) {
      if (i == -1) {
        ret_ptr = data;
      } else {
        StringObject *sobj = (StringObject*) obj_instance_of(args_ptr[i], string_base);
        assert(sobj);
        *(char**) data = sobj->value;
        par_ptrs[i] = data;
      }
      data = (char*) data + ((sizeof(char*)>sizeof(long))?sizeof(char*):sizeof(long));
    }
    else assert(false);
  }
  
  void (*sym_fn)() = *(void(**)())&sym_ptr;
  ffi_call(&ffihdl->cif, sym_fn, ret_ptr, par_ptrs);
  if (obj_instance_of_or_equal(ret_type, ffi_sint)) {
    state->result_value = alloc_int(state, *(int*) ret_ptr);
  } else if (obj_instance_of_or_equal(ret_type, ffi_charptr)) {
    state->result_value = alloc_string(state, *(char**) ret_ptr);
  } else assert(false);
}

static void ffi_sym_fn(VMState *state, Object *thisptr, Object *fn, Object **args_ptr, int args_len) {
  Object *root = state->stack_ptr[state->stack_len - 1].context;
  while (root->parent) root = root->parent;
  Object *array_base = object_lookup(root, "array", NULL);
  Object *string_base = object_lookup(root, "string", NULL);
  Object *pointer_base = object_lookup(root, "pointer", NULL);
  Object *ffi = object_lookup(root, "ffi", NULL);
  Object *handle_base = object_lookup(ffi, "handle", NULL);
  Object *type_base = object_lookup(ffi, "type", NULL);
  
  Object *handle_obj = obj_instance_of(thisptr, handle_base);
  Object *handle_ptr_obj = object_lookup(handle_obj, "pointer", NULL);
  PointerObject *handle_ptr = (PointerObject*) obj_instance_of(handle_ptr_obj, pointer_base);
  void *handle = handle_ptr->ptr;
  
  assert(args_len == 3);
  StringObject *fn_name_obj = (StringObject*) obj_instance_of(args_ptr[0], string_base);
  assert(fn_name_obj);
  
  void *fnptr = dlsym(handle, fn_name_obj->value);
  char *error = dlerror();
  if (error != NULL) {
    fprintf(stderr, "dlsym failed: %s\n", error);
    assert(false);
  }
  
  Object *ret_type = obj_instance_of(args_ptr[1], type_base);
  assert(ret_type);
  
  ArrayObject *par_types = (ArrayObject*) obj_instance_of(args_ptr[2], array_base);
  assert(par_types);
  
  ffi_type *ffi_ret_type = type_to_ffi_ptr(ffi, ret_type);
  FFIHandle *ffihdl = malloc(sizeof(FFIHandle) + sizeof(ffi_type*) * par_types->length);
  
  for (int i = 0; i < par_types->length; ++i) {
    Object *sub_type = obj_instance_of(par_types->ptr[i], type_base);
    assert(sub_type);
    ((ffi_type**)(ffihdl + 1))[i] = type_to_ffi_ptr(ffi, sub_type);
  }
  
  ffi_status status = ffi_prep_cif(&ffihdl->cif, FFI_DEFAULT_ABI, par_types->length, ffi_ret_type, (ffi_type**)(ffihdl + 1));
  if (status != FFI_OK) {
    fprintf(stderr, "FFI error: %i\n", status);
    assert(false);
  }
  
  Object *fn_obj = alloc_fn(state, ffi_call_fn);
  fn_obj->flags |= OBJ_IMMUTABLE;
  object_set(fn_obj, "return_type", ret_type);
  object_set(fn_obj, "parameter_types", args_ptr[2]);
  object_set(fn_obj, "_sym_pointer", alloc_ptr(state, fnptr));
  object_set(fn_obj, "_ffi_pointer", alloc_ptr(state, (void*) ffihdl));
  
  state->result_value = fn_obj;
}

Object *create_root(VMState *state) {
  Object *root = alloc_object(state, NULL);
  
  state->root = root;
  state->stack_ptr[state->stack_len - 1].context = root;
  
  GCRootSet pin_root;
  gc_add_roots(state, &root, 1, &pin_root);
  
  object_set(root, "null", NULL);
  
  Object *function_obj = alloc_object(state, NULL);
  function_obj->flags |= OBJ_NOINHERIT;
  object_set(root, "function", function_obj);
  
  Object *closure_obj = alloc_object(state, NULL);
  closure_obj->flags |= OBJ_NOINHERIT;
  object_set(root, "closure", closure_obj);
  
  Object *closure_gc = alloc_custom_gc(state);
  ((CustomGCObject*) closure_gc)->mark_fn = closure_mark_fn;
  object_set(closure_obj, "gc", closure_gc);
  
  Object *bool_obj = alloc_object(state, NULL);
  bool_obj->flags |= OBJ_NOINHERIT;
  object_set(root, "bool", bool_obj);
  object_set(bool_obj, "!", alloc_fn(state, bool_not_fn));
  
  Object *int_obj = alloc_object(state, NULL);
  int_obj->flags |= OBJ_NOINHERIT;
  object_set(root, "int", int_obj);
  object_set(int_obj, "+" , alloc_fn(state, int_add_fn));
  object_set(int_obj, "-" , alloc_fn(state, int_sub_fn));
  object_set(int_obj, "*" , alloc_fn(state, int_mul_fn));
  object_set(int_obj, "/" , alloc_fn(state, int_div_fn));
  object_set(int_obj, "==", alloc_fn(state, int_eq_fn));
  object_set(int_obj, "<" , alloc_fn(state, int_lt_fn));
  object_set(int_obj, ">" , alloc_fn(state, int_gt_fn));
  object_set(int_obj, "<=", alloc_fn(state, int_le_fn));
  object_set(int_obj, ">=", alloc_fn(state, int_ge_fn));
  
  Object *float_obj = alloc_object(state, NULL);
  float_obj->flags |= OBJ_NOINHERIT;
  object_set(root, "float", float_obj);
  object_set(float_obj, "+" , alloc_fn(state, float_add_fn));
  object_set(float_obj, "-" , alloc_fn(state, float_sub_fn));
  object_set(float_obj, "*" , alloc_fn(state, float_mul_fn));
  object_set(float_obj, "/" , alloc_fn(state, float_div_fn));
  object_set(float_obj, "==", alloc_fn(state, float_eq_fn));
  object_set(float_obj, "<" , alloc_fn(state, float_lt_fn));
  object_set(float_obj, ">" , alloc_fn(state, float_gt_fn));
  object_set(float_obj, "<=", alloc_fn(state, float_le_fn));
  object_set(float_obj, ">=", alloc_fn(state, float_ge_fn));
  
  Object *string_obj = alloc_object(state, NULL);
  string_obj->flags |= OBJ_NOINHERIT;
  object_set(root, "string", string_obj);
  object_set(string_obj, "+", alloc_fn(state, string_add_fn));
  
  Object *array_obj = alloc_object(state, NULL);
  array_obj->flags |= OBJ_NOINHERIT;
  object_set(root, "array", array_obj);
  Object *array_gc =  alloc_custom_gc(state);
  ((CustomGCObject*) array_gc)->mark_fn = array_mark_fn;
  object_set(array_obj, "gc", array_gc);
  object_set(array_obj, "resize", alloc_fn(state, array_resize_fn));
  object_set(array_obj, "push", alloc_fn(state, array_push_fn));
  object_set(array_obj, "pop", alloc_fn(state, array_pop_fn));
  object_set(array_obj, "[]", alloc_fn(state, array_index_fn));
  object_set(array_obj, "[]=", alloc_fn(state, array_index_assign_fn));
  
  Object *ptr_obj = alloc_object(state, NULL);
  ptr_obj->flags |= OBJ_NOINHERIT;
  object_set(root, "pointer", ptr_obj);
  
  object_set(root, "print", alloc_fn(state, print_fn));
  
  Object *ffi_obj = alloc_object(state, NULL);
  object_set(root, "ffi", ffi_obj);
  object_set(ffi_obj, "open", alloc_fn(state, ffi_open_fn));
  Object *type_obj = alloc_object(state, NULL);
  object_set(ffi_obj, "type", type_obj);
  Object
    *ffi_sint_obj = alloc_object(state, type_obj),
    *ffi_ptr_obj = alloc_object(state, type_obj),
    *ffi_char_ptr_obj = alloc_object(state, ffi_ptr_obj);
  object_set(ffi_obj, "sint", ffi_sint_obj);
  object_set(ffi_obj, "ptr", ffi_ptr_obj);
  object_set(ffi_obj, "char_ptr", ffi_char_ptr_obj);
  printf("ffi.sint = %p, ptr = %p, char = %p\n", (void*) ffi_sint_obj, (void*) ffi_ptr_obj, (void*) ffi_char_ptr_obj);
  
  Object *handle_obj = alloc_object(state, NULL);
  object_set(ffi_obj, "handle", handle_obj);
  object_set(handle_obj, "pointer", NULL);
  object_set(handle_obj, "sym", alloc_fn(state, ffi_sym_fn));
  
  root->flags |= OBJ_IMMUTABLE;
  gc_remove_roots(state, &pin_root);
  
  return root;
}
