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
  Object *root = state->stack_ptr[state->stack_len - 1].context;
  while (root->parent) root = root->parent;
  Object *string_base = object_lookup(root, "string", NULL);
  Object *ffi = object_lookup(root, "ffi", NULL);
  Object *handle_base = object_lookup(ffi, "handle", NULL);
  
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

static ffi_type *type_to_ffi_ptr(Object *ffi, Object *obj) {
  Object *void_obj = object_lookup(ffi, "void", NULL);
  Object *sint_obj = object_lookup(ffi, "sint", NULL);
  Object *uint_obj = object_lookup(ffi, "uint", NULL);
  Object *float_obj = object_lookup(ffi, "float", NULL);
  Object *double_obj = object_lookup(ffi, "double", NULL);
  Object *uint32_obj = object_lookup(ffi, "uint32", NULL);
  Object *pointer = object_lookup(ffi, "pointer", NULL);
  if (obj_instance_of_or_equal(obj, void_obj)) return &ffi_type_void;
  if (obj_instance_of_or_equal(obj, uint_obj)) return &ffi_type_uint;
  if (obj_instance_of_or_equal(obj, sint_obj)) return &ffi_type_sint;
  if (obj_instance_of_or_equal(obj, float_obj)) return &ffi_type_float;
  if (obj_instance_of_or_equal(obj, double_obj)) return &ffi_type_double;
  if (obj_instance_of_or_equal(obj, uint32_obj)) return &ffi_type_uint32;
  if (obj_instance_of_or_equal(obj, pointer)) return &ffi_type_pointer;
  assert("Unknown type." && false);
}

static void ffi_ptr_dereference(VMState *state, Object *thisptr, Object *fn, Object **args_ptr, int args_len) {
  VM_ASSERT(args_len == 1, "wrong arity: expected 1, got %i", args_len);
  Object *root = state->stack_ptr[state->stack_len - 1].context;
  while (root->parent) root = root->parent;
  Object *pointer_base = object_lookup(root, "pointer", NULL);
  
  Object *ffi = object_lookup(root, "ffi", NULL);
  Object *ffi_type = object_lookup(ffi, "type", NULL);
  Object *ffi_sint = object_lookup(ffi, "sint", NULL);
  Object *ffi_uint8 = object_lookup(ffi, "uint8", NULL);
  // Object *ffi_uint32 = object_lookup(ffi, "uint32", NULL);
  // Object *ffi_pointer = object_lookup(ffi, "pointer", NULL);
  // Object *ffi_charptr = object_lookup(ffi, "char_pointer", NULL);
  
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
  Object *root = state->stack_ptr[state->stack_len - 1].context;
  while (root->parent) root = root->parent;
  Object *int_base = object_lookup(root, "int", NULL);
  Object *float_base = object_lookup(root, "float", NULL);
  Object *array_base = object_lookup(root, "array", NULL);
  Object *string_base = object_lookup(root, "string", NULL);
  Object *pointer_base = object_lookup(root, "pointer", NULL);
  
  Object *ffi = object_lookup(root, "ffi", NULL);
  Object *ffi_void = object_lookup(ffi, "void", NULL);
  Object *ffi_sint = object_lookup(ffi, "sint", NULL);
  Object *ffi_uint = object_lookup(ffi, "uint", NULL);
  Object *ffi_float = object_lookup(ffi, "float", NULL);
  Object *ffi_double = object_lookup(ffi, "double", NULL);
  Object *ffi_uint32 = object_lookup(ffi, "uint32", NULL);
  Object *ffi_pointer = object_lookup(ffi, "pointer", NULL);
  Object *ffi_charptr = object_lookup(ffi, "char_pointer", NULL);
  
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
  
  VM_ASSERT(args_len == par_types_array->length, "FFI arity violated: expected %i, got %i", par_types_array->length, args_len);
  
  int par_len_sum = 0;
  for (int i = -1; i < args_len; ++i) {
    Object *type;
    if (i == -1) type = ret_type;
    else type = par_types_array->ptr[i];
    
    if (obj_instance_of_or_equal(type, ffi_void)) { }
    else if (obj_instance_of_or_equal(type, ffi_sint)) par_len_sum += sizeof(long);
    else if (obj_instance_of_or_equal(type, ffi_uint)) par_len_sum += sizeof(long);
    else if (obj_instance_of_or_equal(type, ffi_float)) par_len_sum += (sizeof(float)>sizeof(long))?sizeof(float):sizeof(long);
    else if (obj_instance_of_or_equal(type, ffi_double)) par_len_sum += (sizeof(double)>sizeof(long))?sizeof(double):sizeof(long);
    else if (obj_instance_of_or_equal(type, ffi_uint32)) par_len_sum += sizeof(long);
    else if (obj_instance_of_or_equal(type, ffi_pointer)) par_len_sum += (sizeof(void*)>sizeof(long))?sizeof(void*):sizeof(long);
    else assert(false);
  }
  
  void *ret_ptr;
  void **par_ptrs = alloca(sizeof(void*) * args_len);
  void *data = alloca(par_len_sum);
  for (int i = -1; i < args_len; ++i) {
    Object *type;
    if (i == -1) type = ret_type;
    else type = par_types_array->ptr[i];
    
    if (obj_instance_of_or_equal(type, ffi_void)) {
      if (i == -1) ret_ptr = data;
      else assert("Void in parameter types??" && false);
    }
    else if (obj_instance_of_or_equal(type, ffi_sint)
      || obj_instance_of_or_equal(type, ffi_uint32)
      || obj_instance_of_or_equal(type, ffi_uint)
    ) {
      if (i == -1) ret_ptr = data;
      else {
        IntObject *iobj = (IntObject*) obj_instance_of(args_ptr[i], int_base);
        assert(iobj);
        *(int*) data = iobj->value;
        par_ptrs[i] = data;
      }
      data = (char*) data + sizeof(long);
    }
    else if (obj_instance_of_or_equal(type, ffi_float)) {
      if (i == -1) ret_ptr = data;
      else {
        FloatObject *fobj = (FloatObject*) obj_instance_of(args_ptr[i], float_base);
        IntObject *iobj = (IntObject*) obj_instance_of(args_ptr[i], int_base);
        assert(fobj || iobj);
        if (fobj) *(float*) data = fobj->value;
        else *(float*) data = iobj->value;
        par_ptrs[i] = data;
      }
      data = (char*) data + ((sizeof(float)>sizeof(long))?sizeof(float):sizeof(long));
    }
    else if (obj_instance_of_or_equal(type, ffi_double)) {
      if (i == -1) ret_ptr = data;
      else {
        FloatObject *fobj = (FloatObject*) obj_instance_of(args_ptr[i], float_base);
        IntObject *iobj = (IntObject*) obj_instance_of(args_ptr[i], int_base);
        assert(fobj || iobj);
        if (fobj) *(double*) data = fobj->value;
        else *(double*) data = iobj->value;
        par_ptrs[i] = data;
      }
      data = (char*) data + ((sizeof(double)>sizeof(long))?sizeof(double):sizeof(long));
    }
    else if (obj_instance_of_or_equal(type, ffi_charptr)) {
      if (i == -1) ret_ptr = data;
      else {
        StringObject *sobj = (StringObject*) obj_instance_of(args_ptr[i], string_base);
        assert(sobj);
        *(char**) data = sobj->value;
        par_ptrs[i] = data;
      }
      data = (char*) data + ((sizeof(char*)>sizeof(long))?sizeof(char*):sizeof(long));
    }
    else if (obj_instance_of_or_equal(type, ffi_pointer)) {
      if (i == -1) ret_ptr = data;
      else {
        PointerObject *pobj = (PointerObject*) obj_instance_of(args_ptr[i], pointer_base);
        assert(pobj);
        *(void**) data = pobj->ptr;
        par_ptrs[i] = data;
      }
      data = (char*) data + ((sizeof(void*)>sizeof(long))?sizeof(void*):sizeof(long));
    }
    else assert(false);
  }
  
  void (*sym_fn)() = *(void(**)())&sym_ptr;
  ffi_call(&ffihdl->cif, sym_fn, ret_ptr, par_ptrs);
  if (obj_instance_of_or_equal(ret_type, ffi_void)) {
    state->result_value = NULL;
  } else if (obj_instance_of_or_equal(ret_type, ffi_sint)) {
    state->result_value = alloc_int(state, *(int*) ret_ptr);
  } else if (obj_instance_of_or_equal(ret_type, ffi_charptr)) {
    state->result_value = alloc_string(state, *(char**) ret_ptr);
  } else if (obj_instance_of_or_equal(ret_type, ffi_pointer)) {
    Object *ptr = alloc_ptr(state, *(void**) ret_ptr);
    object_set(ptr, "dereference", alloc_fn(state, ffi_ptr_dereference));
    state->result_value = ptr;
  } else assert(false);
}

static void ffi_sym_fn(VMState *state, Object *thisptr, Object *fn, Object **args_ptr, int args_len) {
  VM_ASSERT(args_len == 3, "wrong arity: expected 3, got %i", args_len);
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

void ffi_setup_root(VMState *state, Object *root) {
  Object *ffi_obj = alloc_object(state, NULL);
  object_set(root, "ffi", ffi_obj);
  object_set(ffi_obj, "open", alloc_fn(state, ffi_open_fn));
  Object *type_obj = alloc_object(state, NULL);
  object_set(ffi_obj, "type", type_obj);
  Object
    *ffi_void_obj = alloc_object(state, type_obj),
    *ffi_sint_obj = alloc_object(state, type_obj),
    *ffi_uint_obj = alloc_object(state, type_obj),
    *ffi_uint8_obj = alloc_object(state, type_obj),
    *ffi_uint32_obj = alloc_object(state, type_obj),
    *ffi_float_obj = alloc_object(state, type_obj),
    *ffi_double_obj = alloc_object(state, type_obj),
    *ffi_ptr_obj = alloc_object(state, type_obj),
    *ffi_char_ptr_obj = alloc_object(state, ffi_ptr_obj);
  object_set(ffi_obj, "void", ffi_void_obj);
  object_set(ffi_obj, "sint", ffi_sint_obj);
  object_set(ffi_obj, "uint", ffi_uint_obj);
  object_set(ffi_obj, "uint8", ffi_uint8_obj);
  object_set(ffi_obj, "float", ffi_float_obj);
  object_set(ffi_obj, "double", ffi_double_obj);
  object_set(ffi_obj, "uint32", ffi_uint32_obj);
  object_set(ffi_obj, "pointer", ffi_ptr_obj);
  object_set(ffi_obj, "char_pointer", ffi_char_ptr_obj);
  
  Object *handle_obj = alloc_object(state, NULL);
  object_set(ffi_obj, "handle", handle_obj);
  object_set(handle_obj, "pointer", NULL);
  object_set(handle_obj, "sym", alloc_fn(state, ffi_sym_fn));
}
