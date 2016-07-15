#include "vm/ffi.h"
#include "object.h"

#include <dlfcn.h>
#include <stdio.h>
#include <stdint.h>
#include <alloca.h>

typedef struct {
  ffi_cif cif;
} FFIHandle;


static void ffi_open_fn(VMState *state, Object *thisptr, Object *fn, Object **args_ptr, int args_len) {
  VM_ASSERT(args_len == 1, "wrong arity: expected 1, got %i", args_len);
  Object *root = state->root;
  Object *string_base = state->shared->vcache.string_base;
  Object *ffi = OBJECT_LOOKUP_STRING(root, "ffi", NULL);
  Object *handle_base = OBJECT_LOOKUP_STRING(ffi, "handle", NULL);
  
  StringObject *sarg = (StringObject*) obj_instance_of(args_ptr[0], string_base);
  VM_ASSERT(sarg, "argument to ffi.open must be string!");
  
  char *file = sarg->value;
  void *dlptr = dlopen(file, RTLD_LAZY);
  VM_ASSERT(dlptr, "dlopen failed: %s", dlerror());
  
  Object *handle_obj = alloc_object(state, handle_base);
  handle_obj->flags |= OBJ_FROZEN;
  object_set(handle_obj, "pointer", alloc_ptr(state, dlptr));
  state->result_value = handle_obj;
}

static ffi_type *type_to_ffi_ptr(Object *ffi_obj, Object *obj) {
  FFIObject *ffi = (FFIObject*) ffi_obj;
  if (obj == ffi->void_obj) return &ffi_type_void;
  if (obj == ffi->short_obj) return &ffi_type_sshort;
  if (obj == ffi->ushort_obj) return &ffi_type_ushort;
  if (obj == ffi->int_obj) return &ffi_type_sint;
  if (obj == ffi->uint_obj) return &ffi_type_uint;
  if (obj == ffi->long_obj) return &ffi_type_slong;
  if (obj == ffi->ulong_obj) return &ffi_type_ulong;
  if (obj == ffi->float_obj) return &ffi_type_float;
  if (obj == ffi->double_obj) return &ffi_type_double;
  if (obj == ffi->int8_obj) return &ffi_type_sint8;
  if (obj == ffi->int16_obj) return &ffi_type_sint16;
  if (obj == ffi->int32_obj) return &ffi_type_sint32;
  if (obj == ffi->int64_obj) return &ffi_type_sint64;
  if (obj == ffi->uint8_obj) return &ffi_type_uint8;
  if (obj == ffi->uint16_obj) return &ffi_type_uint16;
  if (obj == ffi->uint32_obj) return &ffi_type_uint32;
  if (obj == ffi->uint64_obj) return &ffi_type_uint64;
  if (obj == ffi->char_pointer_obj) return &ffi_type_pointer;
  if (obj == ffi->pointer_obj) return &ffi_type_pointer;
  abort();
}

static Object *make_ffi_pointer(VMState *state, void *ptr);

static void ffi_ptr_dereference(VMState *state, Object *thisptr, Object *fn, Object **args_ptr, int args_len) {
  VM_ASSERT(args_len == 2, "wrong arity: expected 2, got %i", args_len);
  Object *root = state->root;
  Object *int_base = state->shared->vcache.int_base;
  Object *pointer_base = state->shared->vcache.pointer_base;
  
  // TODO grab direct from ffi object
  Object *ffi = OBJECT_LOOKUP_STRING(root, "ffi", NULL);
  Object *ffi_type = OBJECT_LOOKUP_STRING(ffi, "type", NULL);
  Object *ffi_short = OBJECT_LOOKUP_STRING(ffi, "short", NULL);
  Object *ffi_int = OBJECT_LOOKUP_STRING(ffi, "int", NULL);
  Object *ffi_ushort = OBJECT_LOOKUP_STRING(ffi, "ushort", NULL);
  Object *ffi_int8 = OBJECT_LOOKUP_STRING(ffi, "int8", NULL);
  Object *ffi_uint8 = OBJECT_LOOKUP_STRING(ffi, "uint8", NULL);
  // Object *ffi_uint32 = OBJECT_LOOKUP_STRING(ffi, "uint32", NULL);
  Object *ffi_pointer = OBJECT_LOOKUP_STRING(ffi, "pointer", NULL);
  Object *ffi_charptr = OBJECT_LOOKUP_STRING(ffi, "char_pointer", NULL);
  
  VM_ASSERT(thisptr->parent == pointer_base, "internal error");
  PointerObject *thisptr_obj = (PointerObject*) thisptr;
  
  Object *ffi_type_obj = obj_instance_of(args_ptr[0], ffi_type);
  Object *offs_obj = args_ptr[1];
  VM_ASSERT(offs_obj->parent == int_base, "offset must be integer");
  int offs = ((IntObject*) offs_obj)->value;
  assert(ffi_type_obj);
  char *offset_ptr = (char*) thisptr_obj->ptr + offs;
  if (obj_instance_of_or_equal(ffi_type_obj, ffi_short)) {
    short s = *(short*) offset_ptr;
    state->result_value = alloc_int(state, s);
  } else if (obj_instance_of_or_equal(ffi_type_obj, ffi_ushort)) {
    unsigned short us = *(unsigned short*) offset_ptr;
    state->result_value = alloc_int(state, us);
  } else if (obj_instance_of_or_equal(ffi_type_obj, ffi_int)) {
    int i = *(int*) offset_ptr;
    state->result_value = alloc_int(state, i);
  } else if (obj_instance_of_or_equal(ffi_type_obj, ffi_uint8)) {
    uint8_t u8 = *(uint8_t*) offset_ptr;
    state->result_value = alloc_int(state, u8);
  } else if (obj_instance_of_or_equal(ffi_type_obj, ffi_int8)) {
    int8_t i8 = *(int8_t*) offset_ptr;
    state->result_value = alloc_int(state, i8);
  } else if (obj_instance_of_or_equal(ffi_type_obj, ffi_pointer)) {
    void *ptr = *(void**) offset_ptr;
    state->result_value = make_ffi_pointer(state, ptr);
  } else if (obj_instance_of_or_equal(ffi_type_obj, ffi_charptr)) {
    char *ptr = *(char**) offset_ptr;
    state->result_value = alloc_string_foreign(state, ptr);
  } else assert("TODO" && false);
}

bool ffi_pointer_write(VMState *state, Object *type, void *ptr, Object *value_obj) {
  ValueCache *vcache = &state->shared->vcache;
  Object *string_base = vcache->string_base;
  FFIObject *ffi = (FFIObject*) vcache->ffi_obj;
  if (type == ffi->float_obj) {
    if (value_obj->parent == vcache->float_base) {
      *(float*) ptr = ((FloatObject*) value_obj)->value;
    } else if (value_obj->parent == vcache->int_base) {
      *(float*) ptr = ((IntObject*) value_obj)->value;
    } else {
      VM_ASSERT(false, "invalid value for float type") false;
    }
    return true;
  } else {
    Object *c_type_obj = OBJECT_LOOKUP_STRING(type, "c_type", NULL);
    StringObject *c_type = (StringObject*) obj_instance_of_or_equal(c_type_obj, string_base);
    assert(c_type);
    VM_ASSERT(false, "unhandled pointer write type: %s", c_type->value) false;
  }
}

Object *ffi_pointer_read(VMState *state, Object *type, void *ptr) {
  ValueCache *vcache = &state->shared->vcache;
  Object *string_base = vcache->string_base;
  FFIObject *ffi = (FFIObject*) vcache->ffi_obj;
  if (type == ffi->float_obj) {
    float f = *(float*) ptr;
    return alloc_float(state, f);
  } else if (type == ffi->uint_obj) {
    unsigned int i = *(unsigned int*) ptr;
    return alloc_int(state, i);
  } else {
    Object *c_type_obj = OBJECT_LOOKUP_STRING(type, "c_type", NULL);
    StringObject *c_type = (StringObject*) obj_instance_of_or_equal(c_type_obj, string_base);
    VM_ASSERT(c_type, "internal type error") NULL;
    VM_ASSERT(false, "unhandled pointer read type: %s", c_type->value) NULL;
  }
}

static void ffi_ptr_index_fn(VMState *state, Object *thisptr, Object *fn, Object **args_ptr, int args_len) {
  VM_ASSERT(args_len == 1, "wrong arity: expected 1, got %i", args_len);
  Object *int_base = state->shared->vcache.int_base;
  Object *pointer_base = state->shared->vcache.pointer_base;
  
  VM_ASSERT(thisptr->parent == pointer_base, "invalid pointer index on non-pointer object");
  PointerObject *thisptr_obj = (PointerObject*) thisptr;
  
  Object *ffi_type_obj = OBJECT_LOOKUP_STRING(thisptr, "target_type", NULL);
  VM_ASSERT(ffi_type_obj, "cannot index read on untyped pointer!");
  
  Object *offs_obj = args_ptr[0];
  VM_ASSERT(offs_obj->parent == int_base, "offset must be integer");
  int offs = ((IntObject*) offs_obj)->value;
  
  IntObject *sizeof_obj = (IntObject*) OBJECT_LOOKUP_STRING(ffi_type_obj, "sizeof", NULL);
  VM_ASSERT(sizeof_obj && sizeof_obj->base.parent == int_base, "internal error: sizeof wrong type or undefined");
  int elemsize = sizeof_obj->value;
  
  char *offset_ptr = (char*) thisptr_obj->ptr + elemsize * offs;
  
  state->result_value = ffi_pointer_read(state, ffi_type_obj, (void*) offset_ptr);
}

static void ffi_ptr_index_assign_fn(VMState *state, Object *thisptr, Object *fn, Object **args_ptr, int args_len) {
  VM_ASSERT(args_len == 2, "wrong arity: expected 2, got %i", args_len);
  Object *int_base = state->shared->vcache.int_base;
  Object *pointer_base = state->shared->vcache.pointer_base;
  
  VM_ASSERT(thisptr->parent == pointer_base, "invalid pointer index write on non-pointer object");
  PointerObject *thisptr_obj = (PointerObject*) thisptr;
  
  Object *ffi_type_obj = OBJECT_LOOKUP_STRING(thisptr, "target_type", NULL);
  VM_ASSERT(ffi_type_obj, "cannot assign index on untyped pointer!");
  
  Object *offs_obj = args_ptr[0];
  VM_ASSERT(offs_obj->parent == int_base, "offset must be integer");
  int offs = ((IntObject*) offs_obj)->value;
  
  IntObject *sizeof_obj = (IntObject*) OBJECT_LOOKUP_STRING(ffi_type_obj, "sizeof", NULL);
  VM_ASSERT(sizeof_obj && sizeof_obj->base.parent == int_base, "internal error: sizeof wrong type or undefined");
  int elemsize = sizeof_obj->value;
  
  char *offset_ptr = (char*) thisptr_obj->ptr + elemsize * offs;
  
  bool res = ffi_pointer_write(state, ffi_type_obj, (void*) offset_ptr, args_ptr[1]);
  if (!res) return;
  
  state->result_value = NULL;
}

static void ffi_ptr_add(VMState *state, Object *thisptr, Object *fn, Object **args_ptr, int args_len) {
  VM_ASSERT(args_len == 1, "wrong arity: expected 1, got %i", args_len);
  Object *int_base = state->shared->vcache.int_base;
  Object *pointer_base = state->shared->vcache.pointer_base;
  
  VM_ASSERT(thisptr->parent == pointer_base, "internal error");
  PointerObject *thisptr_obj = (PointerObject*) thisptr;
  void *ptr = (void*) thisptr_obj->ptr;
  
  IntObject *offset_obj = (IntObject*) args_ptr[0];
  VM_ASSERT(offset_obj->base.parent == int_base, "offset must be integer");
  int offset = offset_obj->value;
  
  state->result_value = make_ffi_pointer(state, (void*) ((char*)ptr + offset));
}

static Object *make_ffi_pointer(VMState *state, void *ptr) {
  Object *ptr_obj = alloc_ptr(state, ptr);
  object_set(ptr_obj, "dereference", alloc_fn(state, ffi_ptr_dereference));
  object_set(ptr_obj, "+", alloc_fn(state, ffi_ptr_add));
  object_set(ptr_obj, "target_type", NULL);
  object_set(ptr_obj, "[]", alloc_fn(state, ffi_ptr_index_fn));
  object_set(ptr_obj, "[]=", alloc_fn(state, ffi_ptr_index_assign_fn));
  return ptr_obj;
}

typedef struct {
  FunctionObject base;
  Object *return_type;
  ArrayObject *par_types_array;
  Object *_sym_pointer, *_ffi_pointer;
  int par_len_sum_precomp;
} FFIFunctionObject;

static int ffi_par_len(Object *ret_type, ArrayObject *par_types_array, FFIObject *ffi) {
  int par_len_sum = 0;
  for (int i = -1; i < par_types_array->length; ++i) {
    Object *type;
    if (i == -1) type = ret_type;
    else type = par_types_array->ptr[i];
    
    if (type == ffi->void_obj) { }
#define TYPESZ(T) ((sizeof(T)>sizeof(long))?sizeof(T):sizeof(long))
    else if (type == ffi->short_obj) par_len_sum += TYPESZ(short);
    else if (type == ffi->ushort_obj) par_len_sum += TYPESZ(unsigned short);
    else if (type == ffi->int_obj) par_len_sum += TYPESZ(int);
    else if (type == ffi->uint_obj) par_len_sum += TYPESZ(unsigned int);
    else if (type == ffi->long_obj) par_len_sum += TYPESZ(long);
    else if (type == ffi->ulong_obj) par_len_sum += TYPESZ(unsigned long);
    else if (type == ffi->float_obj) par_len_sum += TYPESZ(float);
    else if (type == ffi->double_obj) par_len_sum += TYPESZ(double);
    else if (type == ffi->int8_obj) par_len_sum += TYPESZ(int8_t);
    else if (type == ffi->int16_obj) par_len_sum += TYPESZ(int16_t);
    else if (type == ffi->int32_obj) par_len_sum += TYPESZ(int32_t);
    else if (type == ffi->int64_obj) par_len_sum += TYPESZ(int64_t);
    else if (type == ffi->uint8_obj) par_len_sum += TYPESZ(uint8_t);
    else if (type == ffi->uint16_obj) par_len_sum += TYPESZ(uint16_t);
    else if (type == ffi->uint32_obj) par_len_sum += TYPESZ(uint32_t);
    else if (type == ffi->uint64_obj) par_len_sum += TYPESZ(uint64_t);
    else if (type == ffi->char_pointer_obj) par_len_sum += TYPESZ(char*);
    else if (type == ffi->pointer_obj) par_len_sum += TYPESZ(void*);
    else abort();
#undef TYPESZ
  }
  return par_len_sum;
}

static void ffi_call_fn(VMState *state, Object *thisptr, Object *fn, Object **args_ptr, int args_len) {
  Object *int_base = state->shared->vcache.int_base;
  Object *float_base = state->shared->vcache.float_base;
  Object *string_base = state->shared->vcache.string_base;
  Object *pointer_base = state->shared->vcache.pointer_base;
  Object *function_base = state->shared->vcache.function_base;
  
  Object *ffi_obj = state->shared->vcache.ffi_obj;
  FFIObject *ffi = (FFIObject*) ffi_obj;
  
  FFIFunctionObject *ffi_fn = (FFIFunctionObject*) obj_instance_of(fn, function_base);
  
  Object *ret_type = ffi_fn->return_type;
  ArrayObject *par_types_array = ffi_fn->par_types_array;
  Object *ffi_ptr_obj = ffi_fn->_ffi_pointer;
  Object *sym_ptr_obj = ffi_fn->_sym_pointer;
  assert(ret_type && par_types_array && ffi_ptr_obj && sym_ptr_obj);
  assert(ffi_ptr_obj->parent == pointer_base && sym_ptr_obj->parent == pointer_base);
  PointerObject *ffi_ptr_obj_sub = (PointerObject*) ffi_ptr_obj;
  PointerObject *sym_ptr_obj_sub = (PointerObject*) sym_ptr_obj;
  FFIHandle *ffihdl = ffi_ptr_obj_sub->ptr;
  void *sym_ptr = sym_ptr_obj_sub->ptr;
  
  VM_ASSERT(args_len == par_types_array->length, "FFI arity violated: expected %i, got %i", par_types_array->length, args_len);
  
  void **par_ptrs = alloca(sizeof(void*) * args_len);
  void *data = alloca(ffi_fn->par_len_sum_precomp);
  void *ret_ptr = data;
  
  if (ret_type == ffi->void_obj) {
  } else if (ret_type == ffi->int_obj || ret_type == ffi->uint_obj
    || ret_type == ffi->int32_obj || ret_type == ffi->uint32_obj
    || ret_type == ffi->long_obj || ret_type == ffi->ulong_obj
  ) { // all types that are <= long
    data = (char*) data + sizeof(long);
  } else if (ret_type == ffi->int64_obj || ret_type == ffi->uint64_obj) {
    data = (char*) data + sizeof(int64_t);
  } else if (ret_type == ffi->float_obj) {
    data = (char*) data + ((sizeof(float)>sizeof(long))?sizeof(float):sizeof(long));
  } else if (ret_type == ffi->double_obj) {
    data = (char*) data + ((sizeof(double)>sizeof(long))?sizeof(double):sizeof(long));
  } else if (ret_type == ffi->char_pointer_obj || ret_type == ffi->pointer_obj) {
    data = (char*) data + sizeof(void*);
  } else abort();
  // fprintf(stderr, "::");
  for (int i = 0; i < args_len; ++i) {
    Object *type = par_types_array->ptr[i];
    
    if (UNLIKELY(type == ffi->void_obj)) {
      VM_ASSERT(false, "void in parameter types??");
    }
    else if (type == ffi->float_obj) {
      // fprintf(stderr, "f");
      Object *obj = args_ptr[i];
      if (obj->parent == float_base) *(float*) data = ((FloatObject*) obj)->value;
      else {
        if (obj->parent == int_base) *(float*) data = ((IntObject*) obj)->value;
        else {
          VM_ASSERT(false, "ffi float argument must be int or float");
        }
      }
      par_ptrs[i] = data;
      data = (char*) data + ((sizeof(float)>sizeof(long))?sizeof(float):sizeof(long));
    }
    else if (type == ffi->int_obj || type == ffi->uint_obj || type == ffi->int32_obj || type == ffi->uint32_obj) {
      // fprintf(stderr, "i32");
      Object *obj = args_ptr[i];
      VM_ASSERT(obj->parent == int_base, "ffi int argument must be int");
      *(int*) data = ((IntObject*) obj)->value;
      par_ptrs[i] = data;
      data = (char*) data + sizeof(long);
    }
    else if (type == ffi->long_obj || type == ffi->ulong_obj) {
      // fprintf(stderr, "l");
      Object *obj = args_ptr[i];
      VM_ASSERT(obj->parent == int_base, "ffi long argument must be int");
      *(long*) data = ((IntObject*) obj)->value;
      par_ptrs[i] = data;
      data = (char*) data + sizeof(long);
    }
    else if (type == ffi->int64_obj || type == ffi->uint64_obj) {
      // fprintf(stderr, "i64");
      Object *obj = args_ptr[i];
      VM_ASSERT(obj->parent == int_base, "ffi (u)int64 argument must be int");
      *(int64_t*) data = ((IntObject*) obj)->value;
      par_ptrs[i] = data;
      data = (char*) data + sizeof(int64_t);
    }
    else if (type == ffi->double_obj) {
      // fprintf(stderr, "d");
      Object *obj = args_ptr[i];
      if (obj->parent == float_base) *(double*) data = ((FloatObject*) obj)->value;
      else {
        if (obj->parent == int_base) *(double*) data = ((IntObject*) obj)->value;
        else {
          VM_ASSERT(false, "ffi double argument must be int or float");
        }
      }
      par_ptrs[i] = data;
      data = (char*) data + ((sizeof(double)>sizeof(long))?sizeof(double):sizeof(long));
    }
    else if (type == ffi->char_pointer_obj) {
      // fprintf(stderr, "pc");
      if (!args_ptr[i]) *(char**) data = NULL;
      else {
        StringObject *sobj = (StringObject*) obj_instance_of(args_ptr[i], string_base);
        VM_ASSERT(sobj, "ffi char* argument must be string");
        *(char**) data = sobj->value;
      }
      par_ptrs[i] = data;
      data = (char*) data + ((sizeof(char*)>sizeof(long))?sizeof(char*):sizeof(long));
    }
    else if (type == ffi->pointer_obj) {
      // fprintf(stderr, "p");
      if (args_ptr[i] == NULL) {
        *(void**) data = NULL;
      } else {
        VM_ASSERT(args_ptr[i]->parent == pointer_base, "ffi pointer argument %i must be pointer", i);
        PointerObject *pobj = (PointerObject*) args_ptr[i];
        *(void**) data = pobj->ptr;
      }
      par_ptrs[i] = data;
      data = (char*) data + ((sizeof(void*)>sizeof(long))?sizeof(void*):sizeof(long));
    }
    else abort();
  }
  
  void (*sym_fn)() = *(void(**)())&sym_ptr;
  ffi_call(&ffihdl->cif, sym_fn, ret_ptr, par_ptrs);
  // fprintf(stderr, " -> ");
  if (ret_type == ffi->void_obj) {
    // fprintf(stderr, "v");
    state->result_value = NULL;
  } else if (ret_type == ffi->int_obj) {
    // fprintf(stderr, "i");
    state->result_value = alloc_int(state, *(int*) ret_ptr);
  } else if (ret_type == ffi->uint_obj) {
    // fprintf(stderr, "u");
    state->result_value = alloc_int(state, *(unsigned int*) ret_ptr);
  } else if (ret_type == ffi->uint32_obj) {
    // fprintf(stderr, "i32");
    state->result_value = alloc_int(state, *(uint32_t*) ret_ptr);
  } else if (ret_type == ffi->char_pointer_obj) {
    // fprintf(stderr, "pc");
    state->result_value = alloc_string(state, *(char**) ret_ptr, strlen(*(char**) ret_ptr));
  } else if (ret_type == ffi->pointer_obj) {
    // fprintf(stderr, "p");
    state->result_value = make_ffi_pointer(state, *(void**) ret_ptr);
  } else if (ret_type == ffi->float_obj) {
    // fprintf(stderr, "f");
    state->result_value = alloc_float(state, *(float*) ret_ptr);
  } else if (ret_type == ffi->double_obj) {
    // fprintf(stderr, "d");
    // TODO alloc_double?
    state->result_value = alloc_float(state, (float) *(double*) ret_ptr);
  } else VM_ASSERT(false, "unknown return type");
  // fprintf(stderr, "\n");
}

static void ffi_call_fn_special_d_d(VMState *state, Object *thisptr, Object *fn, Object **args_ptr, int args_len) {
  Object *int_base = state->shared->vcache.int_base;
  Object *float_base = state->shared->vcache.float_base;
  Object *pointer_base = state->shared->vcache.pointer_base; (void) pointer_base;
  Object *function_base = state->shared->vcache.function_base;
  
  FFIFunctionObject *ffi_fn = (FFIFunctionObject*) obj_instance_of(fn, function_base);
  
  Object *ffi_ptr_obj = ffi_fn->_ffi_pointer;
  Object *sym_ptr_obj = ffi_fn->_sym_pointer;
  assert(ffi_ptr_obj->parent == pointer_base && sym_ptr_obj->parent == pointer_base);
  PointerObject *ffi_ptr_obj_sub = (PointerObject*) ffi_ptr_obj;
  PointerObject *sym_ptr_obj_sub = (PointerObject*) sym_ptr_obj;
  FFIHandle *ffihdl = ffi_ptr_obj_sub->ptr;
  void *sym_ptr = sym_ptr_obj_sub->ptr;
  
  VM_ASSERT(args_len == 1, "FFI arity violated: expected 1, got %i", args_len);
  
  void **par_ptrs = alloca(sizeof(void*) * 1);
  void *data = alloca(sizeof(double));
  void *ret_ptr = alloca(sizeof(double));
  
  Object *arg0 = args_ptr[0];
  if (arg0->parent == float_base) *(double*) data = ((FloatObject*) arg0)->value;
  else {
    if (arg0->parent == int_base) *(double*) data = ((IntObject*) arg0)->value;
    else {
      VM_ASSERT(false, "ffi double argument must be int or float");
    }
  }
  par_ptrs[0] = data;
  
  void (*sym_fn)() = *(void(**)())&sym_ptr;
  ffi_call(&ffihdl->cif, sym_fn, ret_ptr, par_ptrs);
  // TODO alloc_double?
  state->result_value = alloc_float(state, (float) *(double*) ret_ptr);
}

static void ffi_call_fn_special_fx_v(VMState *state, Object *thisptr, Object *fn, Object **args_ptr, int args_len) {
  Object *int_base = state->shared->vcache.int_base;
  Object *float_base = state->shared->vcache.float_base;
  Object *pointer_base = state->shared->vcache.pointer_base; (void) pointer_base;
  Object *function_base = state->shared->vcache.function_base;
  
  FFIFunctionObject *ffi_fn = (FFIFunctionObject*) obj_instance_of(fn, function_base);
  
  ArrayObject *par_types_array = ffi_fn->par_types_array;
  Object *ffi_ptr_obj = ffi_fn->_ffi_pointer;
  Object *sym_ptr_obj = ffi_fn->_sym_pointer;
  assert(ffi_ptr_obj->parent == pointer_base && sym_ptr_obj->parent == pointer_base);
  PointerObject *ffi_ptr_obj_sub = (PointerObject*) ffi_ptr_obj;
  PointerObject *sym_ptr_obj_sub = (PointerObject*) sym_ptr_obj;
  FFIHandle *ffihdl = ffi_ptr_obj_sub->ptr;
  void *sym_ptr = sym_ptr_obj_sub->ptr;
  
  VM_ASSERT(args_len == par_types_array->length, "FFI arity violated: expected %i, got %i", par_types_array->length, args_len);
  
  void **par_ptrs = alloca(sizeof(void*) * 3);
  void *data = alloca(sizeof(float) * 3);
  void *ret_ptr = NULL;
  
  for (int i = 0; i < args_len; ++i) {
    Object *arg = args_ptr[i];
    if (arg->parent == float_base) *(float*) data = ((FloatObject*) arg)->value;
    else {
      if (arg->parent == int_base) *(float*) data = ((IntObject*) arg)->value;
      else {
        VM_ASSERT(false, "ffi float argument %i must be int or float", i);
      }
    }
    par_ptrs[i] = data;
    data = (void*) ((float*) data + 1);
  }
  
  void (*sym_fn)() = *(void(**)())&sym_ptr;
  ffi_call(&ffihdl->cif, sym_fn, ret_ptr, par_ptrs);
  state->result_value = NULL;
}

VMFunctionPointer ffi_get_specialized_call_fn(FFIObject *ffi, Object *ret_type, ArrayObject *par_types) {
  if (ret_type == ffi->double_obj
    && par_types->length == 1 && par_types->ptr[0] == ffi->double_obj)
  {
    return ffi_call_fn_special_d_d;
  }
  if (ret_type == ffi->void_obj) {
    bool all_float = true;
    for (int i = 0; i < par_types->length; i++) {
      if (par_types->ptr[i] != ffi->float_obj) {
        all_float = false;
        break;
      }
    }
    if (all_float) {
      return ffi_call_fn_special_fx_v;
    }
  }
  return ffi_call_fn;
}

static void ffi_sym_fn(VMState *state, Object *thisptr, Object *fn, Object **args_ptr, int args_len) {
  VM_ASSERT(args_len == 3, "wrong arity: expected 3, got %i", args_len);
  Object *array_base = state->shared->vcache.array_base;
  Object *string_base = state->shared->vcache.string_base;
  Object *pointer_base = state->shared->vcache.pointer_base;
  Object *ffi = state->shared->vcache.ffi_obj;
  Object *handle_base = OBJECT_LOOKUP_STRING(ffi, "handle", NULL);
  Object *type_base = OBJECT_LOOKUP_STRING(ffi, "type", NULL);
  
  Object *handle_obj = obj_instance_of(thisptr, handle_base);
  Object *handle_ptr_obj = OBJECT_LOOKUP_STRING(handle_obj, "pointer", NULL);
  VM_ASSERT(handle_ptr_obj->parent == pointer_base, "sym handle must be pointer");
  PointerObject *handle_ptr = (PointerObject*) handle_ptr_obj;
  void *handle = handle_ptr->ptr;
  
  StringObject *fn_name_obj = (StringObject*) obj_instance_of(args_ptr[0], string_base);
  VM_ASSERT(fn_name_obj, "symbol name must be string");
  
  void *fnptr = dlsym(handle, fn_name_obj->value);
  char *error = dlerror();
  // VM_ASSERT(!error, "dlsym failed: %s", error);
  if (error) { state->result_value = NULL; return; }
  
  Object *ret_type = obj_instance_of(args_ptr[1], type_base);
  VM_ASSERT(ret_type, "return type must be ffi.type!");
  
  ArrayObject *par_types = (ArrayObject*) obj_instance_of(args_ptr[2], array_base);
  VM_ASSERT(par_types, "parameter type must be array");
  
  ffi_type *ffi_ret_type = type_to_ffi_ptr(ffi, ret_type);
  FFIHandle *ffihdl = malloc(sizeof(FFIHandle) + sizeof(ffi_type*) * par_types->length);
  
  for (int i = 0; i < par_types->length; ++i) {
    Object *sub_type = obj_instance_of(par_types->ptr[i], type_base);
    VM_ASSERT(sub_type, "parameter type %i must be ffi.type!", i);
    ((ffi_type**)(ffihdl + 1))[i] = type_to_ffi_ptr(ffi, sub_type);
  }
  
  ffi_status status = ffi_prep_cif(&ffihdl->cif, FFI_DEFAULT_ABI, par_types->length, ffi_ret_type, (ffi_type**)(ffihdl + 1));
  VM_ASSERT(status == FFI_OK, "FFI error: %i", status);
  
  VMFunctionPointer ffi_spec_fn = ffi_get_specialized_call_fn((FFIObject*) ffi, ret_type, par_types);
  
  Object *fn_obj = alloc_fn_custom(state, ffi_spec_fn, sizeof(FFIFunctionObject));
  FFIFunctionObject *ffi_fn = (FFIFunctionObject*) fn_obj;
  Object *_sym_pointer = alloc_ptr(state, fnptr);
  Object *_ffi_pointer = alloc_ptr(state, (void*) ffihdl);
  
  fn_obj->flags |= OBJ_FROZEN;
  object_set(fn_obj, "return_type", ret_type);
  object_set(fn_obj, "parameter_types", args_ptr[2]);
  object_set(fn_obj, "_sym_pointer", _sym_pointer);
  object_set(fn_obj, "_ffi_pointer", _ffi_pointer);
  ffi_fn->return_type = ret_type;
  // use the array here, since we don't care about any subtypes
  ffi_fn->par_types_array = par_types;
  ffi_fn->_sym_pointer = _sym_pointer;
  ffi_fn->_ffi_pointer = _ffi_pointer;
  ffi_fn->par_len_sum_precomp = ffi_par_len(ret_type, par_types, (FFIObject*) ffi);
  
  state->result_value = fn_obj;
}

void ffi_setup_root(VMState *state, Object *root) {
  FFIObject *ffi = (FFIObject*) alloc_object_internal(state, sizeof(FFIObject));
  Object *ffi_obj = (Object*) ffi;
  ffi_obj->flags |= OBJ_FROZEN;
  
  object_set((Object*) ffi_obj, "open", alloc_fn(state, ffi_open_fn));
  Object *type_obj = alloc_object(state, NULL);
  object_set((Object*) ffi_obj, "type", type_obj);
  
#define DEFINE_TYPE(NAME, T) ffi->NAME ## _obj = alloc_object(state, type_obj); \
  ffi->NAME ## _obj->flags |= OBJ_NOINHERIT; \
  object_set(ffi->NAME ## _obj, "sizeof", alloc_int(state, sizeof(T))); \
  object_set(ffi->NAME ## _obj, "c_type", alloc_string(state, #T, strlen(#T))); \
  object_set(ffi_obj, #NAME, ffi->NAME ## _obj)
  ffi->void_obj = alloc_object(state, type_obj);
  ffi->void_obj->flags |= OBJ_NOINHERIT;
  object_set(ffi_obj, "void", ffi->void_obj);
  DEFINE_TYPE(short, short);
  DEFINE_TYPE(ushort, unsigned short);
  DEFINE_TYPE(int, int);
  DEFINE_TYPE(uint, unsigned int);
  DEFINE_TYPE(long, long);
  DEFINE_TYPE(ulong, unsigned long);
  DEFINE_TYPE(int8, int8_t);
  DEFINE_TYPE(int16, int16_t);
  DEFINE_TYPE(int32, int32_t);
  DEFINE_TYPE(int64, int64_t);
  DEFINE_TYPE(uint8, uint8_t);
  DEFINE_TYPE(uint16, uint16_t);
  DEFINE_TYPE(uint32, uint32_t);
  DEFINE_TYPE(uint64, uint64_t);
  DEFINE_TYPE(float, float);
  DEFINE_TYPE(double, double);
  DEFINE_TYPE(pointer, void*);
  DEFINE_TYPE(char_pointer, char*);
#undef DEFINE_TYPE

  Object *handle_obj = alloc_object(state, NULL);
  object_set(ffi_obj, "handle", handle_obj);
  object_set(handle_obj, "pointer", NULL);
  object_set(handle_obj, "sym", alloc_fn(state, ffi_sym_fn));
  
  object_set(root, "ffi", ffi_obj);
  state->shared->vcache.ffi_obj = ffi_obj;
}
