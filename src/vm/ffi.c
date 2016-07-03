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
  if (obj == ffi->sint_obj) return &ffi_type_sint;
  if (obj == ffi->uint_obj) return &ffi_type_uint;
  if (obj == ffi->long_obj) return &ffi_type_slong;
  if (obj == ffi->ulong_obj) return &ffi_type_ulong;
  if (obj == ffi->float_obj) return &ffi_type_float;
  if (obj == ffi->double_obj) return &ffi_type_double;
  if (obj == ffi->int8_obj) return &ffi_type_uint8;
  if (obj == ffi->int16_obj) return &ffi_type_uint16;
  if (obj == ffi->int32_obj) return &ffi_type_uint32;
  if (obj == ffi->int64_obj) return &ffi_type_uint64;
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
  
  Object *ffi = OBJECT_LOOKUP_STRING(root, "ffi", NULL);
  Object *ffi_type = OBJECT_LOOKUP_STRING(ffi, "type", NULL);
  Object *ffi_sint = OBJECT_LOOKUP_STRING(ffi, "sint", NULL);
  Object *ffi_uint8 = OBJECT_LOOKUP_STRING(ffi, "uint8", NULL);
  // Object *ffi_uint32 = OBJECT_LOOKUP_STRING(ffi, "uint32", NULL);
  Object *ffi_pointer = OBJECT_LOOKUP_STRING(ffi, "pointer", NULL);
  // Object *ffi_charptr = OBJECT_LOOKUP_STRING(ffi, "char_pointer", NULL);
  
  PointerObject *thisptr_obj = (PointerObject*) obj_instance_of(thisptr, pointer_base);
  assert(thisptr);
  
  Object *ffi_type_obj = obj_instance_of(args_ptr[0], ffi_type);
  Object *offs_obj = args_ptr[1];
  VM_ASSERT(offs_obj->parent == int_base, "offset must be integer");
  int offs = ((IntObject*) offs_obj)->value;
  assert(ffi_type_obj);
  if (obj_instance_of_or_equal(ffi_type_obj, ffi_sint)) {
    int i = ((int*) thisptr_obj->ptr)[offs];
    state->result_value = alloc_int(state, i);
  } else if (obj_instance_of_or_equal(ffi_type_obj, ffi_uint8)) {
    uint8_t u8 = ((uint8_t*) thisptr_obj->ptr)[offs];
    state->result_value = alloc_int(state, u8);
  } else if (obj_instance_of_or_equal(ffi_type_obj, ffi_pointer)) {
    void *ptr = ((void**) thisptr_obj->ptr)[offs];
    state->result_value = make_ffi_pointer(state, ptr);
  } else assert("TODO" && false);
}

static Object *make_ffi_pointer(VMState *state, void *ptr) {
  Object *ptr_obj = alloc_ptr(state, ptr);
  object_set(ptr_obj, "dereference", alloc_fn(state, ffi_ptr_dereference));
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
    
    if (obj_instance_of_or_equal(type, ffi->void_obj)) { }
#define TYPESZ(T) ((sizeof(T)>sizeof(long))?sizeof(T):sizeof(long))
    else if (type == ffi->sint_obj) par_len_sum += TYPESZ(int);
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
  PointerObject *ffi_ptr_obj_sub = (PointerObject*) obj_instance_of(ffi_ptr_obj, pointer_base);
  PointerObject *sym_ptr_obj_sub = (PointerObject*) obj_instance_of(sym_ptr_obj, pointer_base);
  assert(ffi_ptr_obj_sub && sym_ptr_obj_sub);
  FFIHandle *ffihdl = ffi_ptr_obj_sub->ptr;
  void *sym_ptr = sym_ptr_obj_sub->ptr;
  
  VM_ASSERT(args_len == par_types_array->length, "FFI arity violated: expected %i, got %i", par_types_array->length, args_len);
  
  void *ret_ptr = NULL;
  void **par_ptrs = alloca(sizeof(void*) * args_len);
  void *data = alloca(ffi_fn->par_len_sum_precomp);
  for (int i = -1; i < args_len; ++i) {
    Object *type;
    if (i == -1) type = ret_type;
    else type = par_types_array->ptr[i];
    
    if (type == ffi->void_obj) {
      if (i == -1) ret_ptr = data;
      else VM_ASSERT(false, "void in parameter types??");
    }
    else if (type == ffi->sint_obj || type == ffi->uint_obj || type == ffi->int32_obj || type == ffi->uint32_obj) {
      if (i == -1) ret_ptr = data;
      else {
        Object *obj = args_ptr[i];
        VM_ASSERT(obj->parent == int_base, "ffi int argument must be int");
        *(int*) data = ((IntObject*) obj)->value;
        par_ptrs[i] = data;
      }
      data = (char*) data + sizeof(long);
    }
    else if (type == ffi->long_obj || type == ffi->ulong_obj) {
      if (i == -1) ret_ptr = data;
      else {
        Object *obj = args_ptr[i];
        VM_ASSERT(obj->parent == int_base, "ffi long argument must be int");
        *(long*) data = ((IntObject*) obj)->value;
        par_ptrs[i] = data;
      }
      data = (char*) data + sizeof(long);
    }
    else if (type == ffi->int64_obj || type == ffi->uint64_obj) {
      if (i == -1) ret_ptr = data;
      else {
        Object *obj = args_ptr[i];
        VM_ASSERT(obj->parent == int_base, "ffi (u)int64 argument must be int");
        *(int64_t*) data = ((IntObject*) obj)->value;
        par_ptrs[i] = data;
      }
      data = (char*) data + sizeof(int64_t);
    }
    else if (type == ffi->float_obj) {
      if (i == -1) ret_ptr = data;
      else {
        Object *obj = args_ptr[i];
        if (obj->parent == float_base) *(float*) data = ((FloatObject*) obj)->value;
        else {
          if (obj->parent == int_base) *(float*) data = ((IntObject*) obj)->value;
          else {
            VM_ASSERT(false, "ffi float argument must be int or float");
          }
        }
        par_ptrs[i] = data;
      }
      data = (char*) data + ((sizeof(float)>sizeof(long))?sizeof(float):sizeof(long));
    }
    else if (type == ffi->double_obj) {
      if (i == -1) ret_ptr = data;
      else {
        Object *obj = args_ptr[i];
        if (obj->parent == float_base) *(double*) data = ((FloatObject*) obj)->value;
        else {
          if (obj->parent == int_base) *(double*) data = ((IntObject*) obj)->value;
          else {
            VM_ASSERT(false, "ffi double argument must be int or float");
          }
        }
        par_ptrs[i] = data;
      }
      data = (char*) data + ((sizeof(double)>sizeof(long))?sizeof(double):sizeof(long));
    }
    else if (type == ffi->char_pointer_obj) {
      if (i == -1) ret_ptr = data;
      else {
        StringObject *sobj = (StringObject*) obj_instance_of(args_ptr[i], string_base);
        VM_ASSERT(sobj, "ffi char* argument must be string");
        *(char**) data = sobj->value;
        par_ptrs[i] = data;
      }
      data = (char*) data + ((sizeof(char*)>sizeof(long))?sizeof(char*):sizeof(long));
    }
    else if (type == ffi->pointer_obj) {
      if (i == -1) ret_ptr = data;
      else {
        if (args_ptr[i] == NULL) {
          *(void**) data = NULL;
        } else {
          PointerObject *pobj = (PointerObject*) obj_instance_of(args_ptr[i], pointer_base);
          VM_ASSERT(pobj, "ffi pointer argument must be pointer");
          *(void**) data = pobj->ptr;
        }
        par_ptrs[i] = data;
      }
      data = (char*) data + ((sizeof(void*)>sizeof(long))?sizeof(void*):sizeof(long));
    }
    else abort();
  }
  
  void (*sym_fn)() = *(void(**)())&sym_ptr;
  ffi_call(&ffihdl->cif, sym_fn, ret_ptr, par_ptrs);
  if (ret_type == ffi->void_obj) {
    state->result_value = NULL;
  } else if (ret_type == ffi->sint_obj) {
    state->result_value = alloc_int(state, *(int*) ret_ptr);
  } else if (ret_type == ffi->uint_obj) {
    state->result_value = alloc_int(state, *(unsigned int*) ret_ptr);
  } else if (ret_type == ffi->uint32_obj) {
    state->result_value = alloc_int(state, *(uint32_t*) ret_ptr);
  } else if (ret_type == ffi->char_pointer_obj) {
    state->result_value = alloc_string(state, *(char**) ret_ptr, strlen(*(char**) ret_ptr));
  } else if (ret_type == ffi->pointer_obj) {
    state->result_value = make_ffi_pointer(state, *(void**) ret_ptr);
  } else if (ret_type == ffi->float_obj) {
    state->result_value = alloc_float(state, *(float*) ret_ptr);
  } else if (ret_type == ffi->double_obj) {
    // TODO alloc_double?
    state->result_value = alloc_float(state, (float) *(double*) ret_ptr);
  } else VM_ASSERT(false, "unknown return type");
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
  PointerObject *handle_ptr = (PointerObject*) obj_instance_of(handle_ptr_obj, pointer_base);
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
  
  Object *fn_obj = alloc_fn_custom(state, ffi_call_fn, sizeof(FFIFunctionObject));
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
  object_set(ffi_obj, #NAME, ffi->NAME ## _obj)
  ffi->void_obj = alloc_object(state, type_obj);
  ffi->void_obj->flags |= OBJ_NOINHERIT;
  object_set(ffi_obj, "void", ffi->void_obj);
  DEFINE_TYPE(sint, int);
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
