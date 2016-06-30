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
  if (obj_instance_of_or_equal(obj, ffi->void_obj)) return &ffi_type_void;
  if (obj_instance_of_or_equal(obj, ffi->uint_obj)) return &ffi_type_uint;
  if (obj_instance_of_or_equal(obj, ffi->sint_obj)) return &ffi_type_sint;
  if (obj_instance_of_or_equal(obj, ffi->float_obj)) return &ffi_type_float;
  if (obj_instance_of_or_equal(obj, ffi->double_obj)) return &ffi_type_double;
  if (obj_instance_of_or_equal(obj, ffi->int8_obj)) return &ffi_type_uint8;
  if (obj_instance_of_or_equal(obj, ffi->int16_obj)) return &ffi_type_uint16;
  if (obj_instance_of_or_equal(obj, ffi->int32_obj)) return &ffi_type_uint32;
  if (obj_instance_of_or_equal(obj, ffi->int64_obj)) return &ffi_type_uint64;
  if (obj_instance_of_or_equal(obj, ffi->uint8_obj)) return &ffi_type_uint8;
  if (obj_instance_of_or_equal(obj, ffi->uint16_obj)) return &ffi_type_uint16;
  if (obj_instance_of_or_equal(obj, ffi->uint32_obj)) return &ffi_type_uint32;
  if (obj_instance_of_or_equal(obj, ffi->uint64_obj)) return &ffi_type_uint64;
  if (obj_instance_of_or_equal(obj, ffi->pointer_obj)) return &ffi_type_pointer;
  abort();
}

static void ffi_ptr_dereference(VMState *state, Object *thisptr, Object *fn, Object **args_ptr, int args_len) {
  VM_ASSERT(args_len == 1, "wrong arity: expected 1, got %i", args_len);
  Object *root = state->root;
  Object *pointer_base = state->shared->vcache.pointer_base;
  
  Object *ffi = OBJECT_LOOKUP_STRING(root, "ffi", NULL);
  Object *ffi_type = OBJECT_LOOKUP_STRING(ffi, "type", NULL);
  Object *ffi_sint = OBJECT_LOOKUP_STRING(ffi, "sint", NULL);
  Object *ffi_uint8 = OBJECT_LOOKUP_STRING(ffi, "uint8", NULL);
  // Object *ffi_uint32 = OBJECT_LOOKUP_STRING(ffi, "uint32", NULL);
  // Object *ffi_pointer = OBJECT_LOOKUP_STRING(ffi, "pointer", NULL);
  // Object *ffi_charptr = OBJECT_LOOKUP_STRING(ffi, "char_pointer", NULL);
  
  PointerObject *thisptr_obj = (PointerObject*) obj_instance_of(thisptr, pointer_base);
  assert(thisptr);
  
  Object *ffi_type_obj = obj_instance_of(args_ptr[0], ffi_type);
  assert(ffi_type_obj);
  if (obj_instance_of_or_equal(ffi_type_obj, ffi_sint)) {
    int i = *(int*) thisptr_obj->ptr;
    state->result_value = alloc_int(state, i);
  } else if (obj_instance_of_or_equal(ffi_type_obj, ffi_uint8)) {
    uint8_t u8 = *(uint8_t*) thisptr_obj->ptr;
    state->result_value = alloc_int(state, u8);
  } else assert("TODO" && false);
}

static void ffi_call_fn(VMState *state, Object *thisptr, Object *fn, Object **args_ptr, int args_len) {
  Object *int_base = state->shared->vcache.int_base;
  Object *float_base = state->shared->vcache.float_base;
  Object *string_base = state->shared->vcache.string_base;
  Object *pointer_base = state->shared->vcache.pointer_base;
  Object *function_base = state->shared->vcache.function_base;
  
  Object *ffi_obj = state->shared->vcache.ffi_obj;
  FFIObject *ffi = (FFIObject*) ffi_obj;
  
  Object *ret_type = OBJECT_LOOKUP_STRING(fn, "return_type", NULL);
  Object *par_types = OBJECT_LOOKUP_STRING(fn, "parameter_types", NULL);
  Object *ffi_ptr_obj = OBJECT_LOOKUP_STRING(fn, "_ffi_pointer", NULL);
  Object *sym_ptr_obj = OBJECT_LOOKUP_STRING(fn, "_sym_pointer", NULL);
  assert(ret_type && par_types && ffi_ptr_obj && sym_ptr_obj);
  ArrayObject *par_types_array = (ArrayObject*) obj_instance_of(par_types, array_base);
  assert(par_types_array);
  PointerObject *ffi_ptr_obj_sub = (PointerObject*) obj_instance_of(ffi_ptr_obj, pointer_base);
  PointerObject *sym_ptr_obj_sub = (PointerObject*) obj_instance_of(sym_ptr_obj, pointer_base);
  assert(ffi_ptr_obj_sub && sym_ptr_obj_sub);
  FFIHandle *ffihdl = ffi_ptr_obj_sub->ptr;
  void *sym_ptr = sym_ptr_obj_sub->ptr;
  
  VM_ASSERT(args_len == par_types_array->length, "FFI arity violated: expected %i, got %i", par_types_array->length, args_len);
  
  int par_len_sum = 0;
  for (int i = -1; i < args_len; ++i) {
    Object *type;
    if (i == -1) type = ret_type;
    else type = par_types_array->ptr[i];
    
    if (obj_instance_of_or_equal(type, ffi->void_obj)) { }
    else if (obj_instance_of_or_equal(type, ffi->sint_obj)) par_len_sum += sizeof(long);
    else if (obj_instance_of_or_equal(type, ffi->uint_obj)) par_len_sum += sizeof(long);
    else if (obj_instance_of_or_equal(type, ffi->float_obj)) par_len_sum += (sizeof(float)>sizeof(long))?sizeof(float):sizeof(long);
    else if (obj_instance_of_or_equal(type, ffi->double_obj)) par_len_sum += (sizeof(double)>sizeof(long))?sizeof(double):sizeof(long);
    else if (obj_instance_of_or_equal(type, ffi->uint32_obj)) par_len_sum += sizeof(long);
    else if (obj_instance_of_or_equal(type, ffi->char_ptr_obj)) par_len_sum += (sizeof(char*)>sizeof(long))?sizeof(char*):sizeof(long);
    else if (obj_instance_of_or_equal(type, ffi->pointer_obj)) par_len_sum += (sizeof(void*)>sizeof(long))?sizeof(void*):sizeof(long);
    else assert(false);
  }
  
  void *ret_ptr = NULL;
  void **par_ptrs = alloca(sizeof(void*) * args_len);
  void *data = alloca(par_len_sum);
  for (int i = -1; i < args_len; ++i) {
    Object *type;
    if (i == -1) type = ret_type;
    else type = par_types_array->ptr[i];
    
    if (obj_instance_of_or_equal(type, ffi->void_obj)) {
      if (i == -1) ret_ptr = data;
      else VM_ASSERT(false, "void in parameter types??");
    }
    else if (obj_instance_of_or_equal(type, ffi->sint_obj)
      || obj_instance_of_or_equal(type, ffi->uint32_obj)
      || obj_instance_of_or_equal(type, ffi->uint_obj)
    ) {
      if (i == -1) ret_ptr = data;
      else {
        IntObject *iobj = (IntObject*) obj_instance_of(args_ptr[i], int_base);
        VM_ASSERT(iobj, "ffi int rgument must be int");
        *(int*) data = iobj->value;
        par_ptrs[i] = data;
      }
      data = (char*) data + sizeof(long);
    }
    else if (obj_instance_of_or_equal(type, ffi->float_obj)) {
      if (i == -1) ret_ptr = data;
      else {
        FloatObject *fobj = (FloatObject*) obj_instance_of(args_ptr[i], float_base);
        IntObject *iobj = (IntObject*) obj_instance_of(args_ptr[i], int_base);
        VM_ASSERT(fobj || iobj, "ffi float argument must be int or float");
        if (fobj) *(float*) data = fobj->value;
        else *(float*) data = iobj->value;
        par_ptrs[i] = data;
      }
      data = (char*) data + ((sizeof(float)>sizeof(long))?sizeof(float):sizeof(long));
    }
    else if (obj_instance_of_or_equal(type, ffi->double_obj)) {
      if (i == -1) ret_ptr = data;
      else {
        FloatObject *fobj = (FloatObject*) obj_instance_of(args_ptr[i], float_base);
        IntObject *iobj = (IntObject*) obj_instance_of(args_ptr[i], int_base);
        VM_ASSERT(fobj || iobj, "ffi double argument must be int or float");
        if (fobj) *(double*) data = fobj->value;
        else *(double*) data = iobj->value;
        par_ptrs[i] = data;
      }
      data = (char*) data + ((sizeof(double)>sizeof(long))?sizeof(double):sizeof(long));
    }
    else if (obj_instance_of_or_equal(type, ffi->char_ptr_obj)) {
      if (i == -1) ret_ptr = data;
      else {
        StringObject *sobj = (StringObject*) obj_instance_of(args_ptr[i], string_base);
        VM_ASSERT(sobj, "ffi char* argument must be string");
        *(char**) data = sobj->value;
        par_ptrs[i] = data;
      }
      data = (char*) data + ((sizeof(char*)>sizeof(long))?sizeof(char*):sizeof(long));
    }
    else if (obj_instance_of_or_equal(type, ffi->pointer_obj)) {
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
  if (obj_instance_of_or_equal(ret_type, ffi->void_obj)) {
    state->result_value = NULL;
  } else if (obj_instance_of_or_equal(ret_type, ffi->sint_obj)) {
    state->result_value = alloc_int(state, *(int*) ret_ptr);
  } else if (obj_instance_of_or_equal(ret_type, ffi->uint_obj)) {
    state->result_value = alloc_int(state, *(unsigned int*) ret_ptr);
  } else if (obj_instance_of_or_equal(ret_type, ffi->uint32_obj)) {
    state->result_value = alloc_int(state, *(uint32_t*) ret_ptr);
  } else if (obj_instance_of_or_equal(ret_type, ffi->char_ptr_obj)) {
    state->result_value = alloc_string(state, *(char**) ret_ptr, strlen(*(char**) ret_ptr));
  } else if (obj_instance_of_or_equal(ret_type, ffi->pointer_obj)) {
    if (*(void**) ret_ptr == NULL) {
      state->result_value = NULL;
    } else {
      Object *ptr = alloc_ptr(state, *(void**) ret_ptr);
      object_set(ptr, "dereference", alloc_fn(state, ffi_ptr_dereference));
      state->result_value = ptr;
    }
  } else if (obj_instance_of_or_equal(ret_type, ffi->float_obj)) {
    state->result_value = alloc_float(state, *(float*) ret_ptr);
  } else if (obj_instance_of_or_equal(ret_type, ffi->double_obj)) {
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
  
  Object *fn_obj = alloc_fn(state, ffi_call_fn);
  fn_obj->flags |= OBJ_FROZEN;
  object_set(fn_obj, "return_type", ret_type);
  object_set(fn_obj, "parameter_types", args_ptr[2]);
  object_set(fn_obj, "_sym_pointer", alloc_ptr(state, fnptr));
  object_set(fn_obj, "_ffi_pointer", alloc_ptr(state, (void*) ffihdl));
  
  state->result_value = fn_obj;
}

void ffi_setup_root(VMState *state, Object *root) {
  FFIObject *ffi = (FFIObject*) alloc_object_internal(state, sizeof(FFIObject));
  Object *ffi_obj = (Object*) ffi;
  object_set((Object*) ffi_obj, "open", alloc_fn(state, ffi_open_fn));
  Object *type_obj = alloc_object(state, NULL);
  object_set((Object*) ffi_obj, "type", type_obj);
  ffi->void_obj = alloc_object(state, type_obj);
  ffi->sint_obj = alloc_object(state, type_obj);
  ffi->uint_obj = alloc_object(state, type_obj);
  ffi->int8_obj = alloc_object(state, type_obj);
  ffi->int16_obj = alloc_object(state, type_obj);
  ffi->int32_obj = alloc_object(state, type_obj);
  ffi->int64_obj = alloc_object(state, type_obj);
  ffi->uint8_obj = alloc_object(state, type_obj);
  ffi->uint16_obj = alloc_object(state, type_obj);
  ffi->uint32_obj = alloc_object(state, type_obj);
  ffi->uint64_obj = alloc_object(state, type_obj);
  ffi->float_obj = alloc_object(state, type_obj);
  ffi->double_obj = alloc_object(state, type_obj);
  ffi->pointer_obj = alloc_object(state, type_obj);
  ffi->char_ptr_obj = alloc_object(state, ffi->pointer_obj);
  
  object_set(ffi_obj, "void", ffi->void_obj);
  object_set(ffi_obj, "sint", ffi->sint_obj);
  object_set(ffi_obj, "uint", ffi->uint_obj);
  object_set(ffi_obj, "float", ffi->float_obj);
  object_set(ffi_obj, "double", ffi->double_obj);
  object_set(ffi_obj, "int8", ffi->int8_obj);
  object_set(ffi_obj, "int16", ffi->int16_obj);
  object_set(ffi_obj, "int32", ffi->int32_obj);
  object_set(ffi_obj, "int64", ffi->int64_obj);
  object_set(ffi_obj, "uint8", ffi->uint8_obj);
  object_set(ffi_obj, "uint16", ffi->uint16_obj);
  object_set(ffi_obj, "uint32", ffi->uint32_obj);
  object_set(ffi_obj, "uint64", ffi->uint64_obj);
  object_set(ffi_obj, "pointer", ffi->pointer_obj);
  object_set(ffi_obj, "char_pointer", ffi->char_ptr_obj);
  
  Object *handle_obj = alloc_object(state, NULL);
  object_set(ffi_obj, "handle", handle_obj);
  object_set(handle_obj, "pointer", NULL);
  object_set(handle_obj, "sym", alloc_fn(state, ffi_sym_fn));
  
  ffi_obj->flags |= OBJ_FROZEN;
  object_set(root, "ffi", ffi_obj);
}
