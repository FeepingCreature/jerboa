#include "vm/ffi.h"
#include "object.h"
#include "util.h"

#include <stdio.h>
#include <stdint.h>
#ifdef _WIN32
  #include <malloc.h>
#else
  #include <alloca.h>
#endif

typedef struct {
  ffi_cif cif;
} FFIHandle;


static void ffi_open_fn(VMState *state, CallInfo *info) {
  VM_ASSERT(info->args_len == 1, "wrong arity: expected 1, got %i", info->args_len);
  Object *root = state->root;
  Object *string_base = state->shared->vcache.string_base;
  Object *array_base = state->shared->vcache.array_base;
  Object *ffi = AS_OBJ(OBJECT_LOOKUP(root, ffi));
  Object *handle_base = AS_OBJ(OBJECT_LOOKUP(ffi, handle));

  StringObject *sarg = (StringObject*) obj_instance_of(OBJ_OR_NULL(load_arg(state->frame, INFO_ARGS_PTR(info)[0])), string_base);
  VM_ASSERT(sarg, "argument to ffi.open must be string!");

  Object *libmap = AS_OBJ(OBJECT_LOOKUP(ffi, library_map));

  char *file = sarg->value;

  bool file_found = false;
  FastKey file_key = prepare_key(file, strlen(file));
  Value mapping = object_lookup_p(libmap, &file_key, &file_found);
  const char **file_list_ptr = NULL;
  int file_list_len = 0;
  if (file_found) {
    ArrayObject *aobj = (ArrayObject*) obj_instance_of(OBJ_OR_NULL(mapping), array_base);
    StringObject *sobj = (StringObject*) obj_instance_of(OBJ_OR_NULL(mapping), string_base);
    if (aobj) {
      file_list_len = aobj->length;
      file_list_ptr = malloc(sizeof(char*) * file_list_len);
      for (int i = 0; i < file_list_len; i++) {
        StringObject *file = (StringObject*) obj_instance_of(OBJ_OR_NULL(aobj->ptr[i]), string_base);
        VM_ASSERT(file, "library_map sub-entries must be string");
        file_list_ptr[i] = my_asprintf("%s", file->value); // outside gc, make copy
      }
    } else if (sobj) {
      file_list_len = 1;
      file_list_ptr = malloc(sizeof(char*) * 1);
      file_list_ptr[0] = my_asprintf("%s", sobj->value);
    } else VM_ASSERT(false, "library_map entries must be string or array");
  } else {
    file_list_len = 1;
    file_list_ptr = malloc(sizeof(char*) * 1);
    file_list_ptr[0] = file;
  }

  void *dlptr = my_dlopen(file_list_len, file_list_ptr);

  Object *handle_obj = AS_OBJ(make_object(state, handle_base, false));
  handle_obj->flags |= OBJ_FROZEN;
  OBJECT_SET(state, handle_obj, pointer, make_ptr(state, dlptr));
  vm_return(state, info, OBJ2VAL(handle_obj));
}

static ffi_type *type_to_ffi_ptr(VMState *state, Object *ffi_obj, Object *obj) {
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
  if (obj == ffi->size_t_obj) {
    if (sizeof(size_t) == 8) return &ffi_type_uint64;
    else if (sizeof(size_t) == 4) return &ffi_type_uint32;
    else abort();
  }
  if (obj == ffi->char_pointer_obj) return &ffi_type_pointer;
  if (obj == ffi->pointer_obj) return &ffi_type_pointer;
  if (obj_instance_of(obj, ffi->struct_obj)) {
    Value complete = OBJECT_LOOKUP(obj, complete);
    VM_ASSERT(value_is_truthy(complete), "cannot use incomplete struct in ffi call") NULL;
    ArrayObject *members = (ArrayObject*) obj_instance_of(OBJ_OR_NULL(OBJECT_LOOKUP(obj, members)), state->shared->vcache.array_base);
    VM_ASSERT(members, "ffi struct members undefined!") NULL;
    ffi_type *str_type = malloc(sizeof(ffi_type));
    *str_type = (ffi_type) {
      .size = 0,
      .alignment = 0,
      .type = FFI_TYPE_STRUCT,
      // TODO just dangle off str_type
      .elements = malloc(sizeof(ffi_type*) * (members->length + 1))
    };
    for (int i = 0; i < members->length; i++) {
      ffi_type *ptr = type_to_ffi_ptr(state, ffi_obj, OBJ_OR_NULL(members->ptr[i]));
      if (!ptr) return NULL; // recursion errored
      str_type->elements[i] = ptr;
    }
    str_type->elements[members->length] = NULL;
    return str_type;
  }
  abort();
}

static Value make_ffi_pointer(VMState *state, void *ptr);

static void ffi_ptr_dereference(VMState *state, CallInfo *info) {
  VM_ASSERT(info->args_len == 2, "wrong arity: expected 2, got %i", info->args_len);
  Object *root = state->root;
  Object *pointer_base = state->shared->vcache.pointer_base;

  FFIObject *ffi = (FFIObject*) AS_OBJ(OBJECT_LOOKUP(root, ffi));
  Object *ffi_type = AS_OBJ(OBJECT_LOOKUP((Object*) ffi, type));

  Object *thisptr = AS_OBJ(load_arg(state->frame, info->this_arg));
  VM_ASSERT(thisptr->parent == pointer_base, "internal error");
  PointerObject *thisptr_obj = (PointerObject*) thisptr;

  Object *ffi_type_obj = obj_instance_of(OBJ_OR_NULL(load_arg(state->frame, INFO_ARGS_PTR(info)[0])), ffi_type);
  Value offs_val = load_arg(state->frame, INFO_ARGS_PTR(info)[1]);
  VM_ASSERT(IS_INT(offs_val), "offset must be integer");
  int offs = AS_INT(offs_val);
  VM_ASSERT(ffi_type_obj, "type is not a FFI type");
  char *offset_ptr = (char*) thisptr_obj->ptr + offs;
  if (ffi_type_obj == ffi->short_obj) {
    short s = *(short*) offset_ptr;
    vm_return(state, info, INT2VAL(s));
  } else if (ffi_type_obj == ffi->ushort_obj) {
    unsigned short us = *(unsigned short*) offset_ptr;
    vm_return(state, info, INT2VAL(us));
  } else if (ffi_type_obj == ffi->int_obj) {
    int i = *(int*) offset_ptr;
    vm_return(state, info, INT2VAL(i));
  } else if (ffi_type_obj == ffi->uint_obj) {
    unsigned int u = *(unsigned int*) offset_ptr;
    vm_return(state, info, INT2VAL(u));
  } else if (ffi_type_obj == ffi->int8_obj) {
    int8_t i8 = *(int8_t*) offset_ptr;
    vm_return(state, info, INT2VAL(i8));
  } else if (ffi_type_obj == ffi->uint8_obj) {
    uint8_t u8 = *(uint8_t*) offset_ptr;
    vm_return(state, info, INT2VAL(u8));
  } else if (ffi_type_obj == ffi->int32_obj) {
    int32_t i32 = *(int32_t*) offset_ptr;
    vm_return(state, info, INT2VAL(i32));
  } else if (ffi_type_obj == ffi->uint32_obj) {
    uint32_t u32 = *(uint32_t*) offset_ptr;
    vm_return(state, info, INT2VAL(u32));
  } else if (ffi_type_obj == ffi->pointer_obj) {
    void *ptr = *(void**) offset_ptr;
    vm_return(state, info, make_ffi_pointer(state, ptr));
  } else if (ffi_type_obj == ffi->char_pointer_obj) {
    char *ptr = *(char**) offset_ptr;
    vm_return(state, info, make_string_static(state, ptr));
  } else if (ffi_type_obj == ffi->long_obj) {
    long l = *(long*) offset_ptr;
    if (l < INT_MIN || l > INT_MAX) {
      VM_ASSERT(false, "value exceeds bounds of my int type");
    }
    vm_return(state, info, INT2VAL((int) l));
  } else { fprintf(stderr, "TODO\n"); abort(); }
}

static void ffi_ptr_dereference_assign(VMState *state, CallInfo *info) {
  VM_ASSERT(info->args_len == 3, "wrong arity: expected 2, got %i", info->args_len);
  Object *root = state->root;
  Object *pointer_base = state->shared->vcache.pointer_base;

  FFIObject *ffi = (FFIObject*) AS_OBJ(OBJECT_LOOKUP(root, ffi));
  Object *ffi_type = AS_OBJ(OBJECT_LOOKUP((Object*) ffi, type));

  Object *thisptr = AS_OBJ(load_arg(state->frame, info->this_arg));
  VM_ASSERT(thisptr->parent == pointer_base, "internal error");
  PointerObject *thisptr_obj = (PointerObject*) thisptr;

  Object *ffi_type_obj = obj_instance_of(OBJ_OR_NULL(load_arg(state->frame, INFO_ARGS_PTR(info)[0])), ffi_type);
  Value offs_val = load_arg(state->frame, INFO_ARGS_PTR(info)[1]);
  VM_ASSERT(IS_INT(offs_val), "offset must be integer");
  int offs = AS_INT(offs_val);
  VM_ASSERT(ffi_type_obj, "type is not a FFI type");
  char *offset_ptr = (char*) thisptr_obj->ptr + offs;

  Value value = load_arg(state->frame, INFO_ARGS_PTR(info)[2]);

  (void) offset_ptr; (void) value;
  if (ffi_type_obj == ffi->long_obj) {
    VM_ASSERT(IS_INT(value), "can only assign integer to long");
    *(long*) offset_ptr = AS_INT(value);
  } else {
    fprintf(stderr, "TODO\n");
    abort();
  }
}

bool ffi_pointer_write(VMState *state, Object *type, void *ptr, Value val) {
  ValueCache *vcache = &state->shared->vcache;
  Object *string_base = vcache->string_base;
  FFIObject *ffi = (FFIObject*) vcache->ffi_obj;
  if (type == ffi->float_obj) {
    if (IS_FLOAT(val)) *(float*) ptr = AS_FLOAT(val);
    else if (IS_INT(val)) *(float*) ptr = AS_INT(val);
    else {
      VM_ASSERT(false, "invalid value for float type") false;
    }
    return true;
  } else {
    Object *c_type_obj = AS_OBJ(OBJECT_LOOKUP(type, c_type));
    StringObject *c_type = (StringObject*) obj_instance_of(c_type_obj, string_base);
    assert(c_type);
    VM_ASSERT(false, "unhandled pointer write type: %s", c_type->value) false;
  }
}

Value ffi_pointer_read(VMState *state, Object *type, void *ptr) {
  ValueCache *vcache = &state->shared->vcache;
  Object *string_base = vcache->string_base;
  FFIObject *ffi = (FFIObject*) vcache->ffi_obj;
  if (type == ffi->float_obj) {
    float f = *(float*) ptr;
    return FLOAT2VAL(f);
  } else if (type == ffi->int_obj) {
    int i = *(int*) ptr;
    return INT2VAL(i);
  } else if (type == ffi->uint_obj) {
    unsigned int i = *(unsigned int*) ptr;
    return INT2VAL(i);
  } else {
    bool has_pointer = false;
    OBJECT_LOOKUP_P(type, pointer, &has_pointer);
    if (!has_pointer) {
      Object *c_type_obj = OBJ_OR_NULL(OBJECT_LOOKUP(type, c_type));
      StringObject *c_type = (StringObject*) obj_instance_of(c_type_obj, string_base);
      VM_ASSERT(c_type, "internal type error") VNULL;
      VM_ASSERT(false, "unhandled pointer read type: %s", c_type->value) VNULL;
    }
    Value res = make_object(state, type, false);
    char *error = OBJECT_SET(state, AS_OBJ(res), pointer, make_ffi_pointer(state, ptr));
    VM_ASSERT(!error, error) VNULL;
    return res;
  }
}

static void ffi_ptr_index_fn(VMState *state, CallInfo *info) {
  VM_ASSERT(info->args_len == 1, "wrong arity: expected 1, got %i", info->args_len);
  Object *pointer_base = state->shared->vcache.pointer_base;
  Object *thisptr = OBJ_OR_NULL(load_arg(state->frame, info->this_arg));

  VM_ASSERT(thisptr && thisptr->parent == pointer_base, "invalid pointer index on non-pointer object");
  PointerObject *thisptr_obj = (PointerObject*) thisptr;

  Value offs_val = load_arg(state->frame, INFO_ARGS_PTR(info)[0]);
  VM_ASSERT(IS_INT(offs_val), "offset must be integer");
  int offs = AS_INT(offs_val);

  Object *ffi_type_obj = OBJ_OR_NULL(OBJECT_LOOKUP(thisptr, target_type));
  VM_ASSERT(ffi_type_obj, "cannot index read on untyped pointer!");

  Value sizeof_val = OBJECT_LOOKUP(ffi_type_obj, sizeof);
  VM_ASSERT(IS_INT(sizeof_val), "internal error: sizeof wrong type or undefined");
  int elemsize = AS_INT(sizeof_val);

  char *offset_ptr = (char*) thisptr_obj->ptr + elemsize * offs;

  Value res = ffi_pointer_read(state, ffi_type_obj, (void*) offset_ptr);
  vm_return(state, info, res);
}

static void ffi_ptr_index_assign_fn(VMState *state, CallInfo *info) {
  VM_ASSERT(info->args_len == 2, "wrong arity: expected 2, got %i", info->args_len);
  Object *pointer_base = state->shared->vcache.pointer_base;
  Object *thisptr = OBJ_OR_NULL(load_arg(state->frame, info->this_arg));

  VM_ASSERT(thisptr && thisptr->parent == pointer_base, "invalid pointer index write on non-pointer object");
  PointerObject *thisptr_obj = (PointerObject*) thisptr;

  Object *ffi_type_obj = AS_OBJ(OBJECT_LOOKUP(thisptr, target_type));
  VM_ASSERT(ffi_type_obj, "cannot assign index on untyped pointer!");

  Value offs_val = load_arg(state->frame, INFO_ARGS_PTR(info)[0]);
  VM_ASSERT(IS_INT(offs_val), "offset must be integer");
  int offs = AS_INT(offs_val);

  Value sizeof_val = OBJECT_LOOKUP(ffi_type_obj, sizeof);
  VM_ASSERT(IS_INT(sizeof_val), "internal error: sizeof wrong type or undefined");
  int elemsize = AS_INT(sizeof_val);

  char *offset_ptr = (char*) thisptr_obj->ptr + elemsize * offs;

  bool res = ffi_pointer_write(state, ffi_type_obj, (void*) offset_ptr, load_arg(state->frame, INFO_ARGS_PTR(info)[1]));
  if (!res) return;
}

static void ffi_ptr_add(VMState *state, CallInfo *info) {
  VM_ASSERT(info->args_len == 1, "wrong arity: expected 1, got %i", info->args_len);
  Object *pointer_base = state->shared->vcache.pointer_base;
  Object *thisptr = OBJ_OR_NULL(load_arg(state->frame, info->this_arg));

  VM_ASSERT(thisptr && thisptr->parent == pointer_base, "internal error");
  PointerObject *thisptr_obj = (PointerObject*) thisptr;
  void *ptr = (void*) thisptr_obj->ptr;

  int elemsize = 1;
  Value target_type = OBJECT_LOOKUP(thisptr, target_type);
  if (!IS_NULL(target_type)) {
    VM_ASSERT(IS_OBJ(target_type), "target type must be ffi type");

    Value sizeof_val = OBJECT_LOOKUP(AS_OBJ(target_type), sizeof);
    VM_ASSERT(IS_INT(sizeof_val), "internal error: sizeof wrong type or undefined");
    elemsize = AS_INT(sizeof_val);
  }

  Value offset_val = load_arg(state->frame, INFO_ARGS_PTR(info)[0]);
  VM_ASSERT(IS_INT(offset_val), "offset must be integer");
  int offset = AS_INT(offset_val);

  vm_return(state, info, make_ffi_pointer(state, (void*) ((char*)ptr + offset * elemsize)));
}

static Value make_ffi_pointer(VMState *state, void *ptr) {
  Object *ptr_obj = AS_OBJ(make_ptr(state, ptr));
  OBJECT_SET(state, ptr_obj, dereference, make_fn(state, ffi_ptr_dereference));
  OBJECT_SET(state, ptr_obj, dereference_assign, make_fn(state, ffi_ptr_dereference_assign));
  OBJECT_SET(state, ptr_obj, __add, make_fn(state, ffi_ptr_add));
  OBJECT_SET(state, ptr_obj, target_type, VNULL);
  OBJECT_SET(state, ptr_obj, __slice, make_fn(state, ffi_ptr_index_fn));
  OBJECT_SET(state, ptr_obj, __slice_assign, make_fn(state, ffi_ptr_index_assign_fn));
  return OBJ2VAL(ptr_obj);
}

typedef struct {
  FunctionObject base;
  Object *return_type;
  ArrayObject *par_types_array;
  Object *_sym_pointer, *_ffi_pointer;
  int par_len_sum_precomp;
} FFIFunctionObject;

static int ffi_par_len(VMState *state, Object *ret_type, ArrayObject *par_types_array, FFIObject *ffi) {
  int par_len_sum = 0;
  for (int i = -1; i < par_types_array->length; ++i) {
    Object *type;
    if (i == -1) type = ret_type;
    else type = AS_OBJ(par_types_array->ptr[i]);

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
    else if (type == ffi->size_t_obj) par_len_sum += TYPESZ(size_t);
    else if (obj_instance_of(type, ffi->struct_obj)) {
      Value struct_sz_val = OBJECT_LOOKUP(type, sizeof);
      VM_ASSERT(IS_INT(struct_sz_val), "sizeof should really have been int") -1;
      int struct_sz = AS_INT(struct_sz_val);
      VM_ASSERT(struct_sz >= 0 && struct_sz < 16384, "wat r u even doing") -1;
      unsigned int struct_sz_u = (unsigned int) struct_sz;
      if (struct_sz_u < sizeof(long)) struct_sz_u = sizeof(long);
      par_len_sum += struct_sz_u;
    }
    else abort();
#undef TYPESZ
  }
  return par_len_sum;
}

static void ffi_call_fn(VMState *state, CallInfo *info) {
  Object *string_base = state->shared->vcache.string_base;
  Object *pointer_base = state->shared->vcache.pointer_base;
  Object *function_base = state->shared->vcache.function_base;

  Object *ffi_obj = state->shared->vcache.ffi_obj;
  FFIObject *ffi = (FFIObject*) ffi_obj;

  FFIFunctionObject *ffi_fn = (FFIFunctionObject*) obj_instance_of(OBJ_OR_NULL(load_arg(state->frame, info->fn)), function_base);

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

  VM_ASSERT(info->args_len == par_types_array->length, "FFI arity violated: expected %i, got %i", par_types_array->length, info->args_len);

  void **par_ptrs = alloca(sizeof(void*) * info->args_len);
  void *data = alloca(ffi_fn->par_len_sum_precomp);
  void *ret_ptr = data;

  if (ret_type == ffi->void_obj) {
  } else if (ret_type == ffi->int_obj || ret_type == ffi->uint_obj
    || ret_type == ffi->int32_obj || ret_type == ffi->uint32_obj
    || ret_type == ffi->long_obj || ret_type == ffi->ulong_obj
    || ret_type == ffi->int8_obj || ret_type == ffi->uint8_obj
    || (sizeof(size_t) == 4 && ret_type == ffi->size_t_obj)
  ) { // all types that are <= long
    data = (char*) data + sizeof(long);
  } else if (ret_type == ffi->int64_obj || ret_type == ffi->uint64_obj
    || (sizeof(size_t) == 8 && ret_type == ffi->size_t_obj)
  ) {
    data = (char*) data + sizeof(int64_t);
  } else if (ret_type == ffi->float_obj) {
    data = (char*) data + ((sizeof(float)>sizeof(long))?sizeof(float):sizeof(long));
  } else if (ret_type == ffi->double_obj) {
    data = (char*) data + ((sizeof(double)>sizeof(long))?sizeof(double):sizeof(long));
  } else if (ret_type == ffi->char_pointer_obj || ret_type == ffi->pointer_obj) {
    data = (char*) data + sizeof(void*);
  } else if (obj_instance_of(ret_type, ffi->struct_obj)) {
    Value struct_sz_val = OBJECT_LOOKUP(ret_type, sizeof);
    VM_ASSERT(IS_INT(struct_sz_val), "sizeof should really have been int");
    int struct_sz = AS_INT(struct_sz_val);
    // NOTE: point of danger??
    VM_ASSERT(struct_sz >= 0 && struct_sz < 16384, "wat r u even doing");
    unsigned int struct_sz_u = struct_sz;
    if (struct_sz_u < sizeof(long)) struct_sz_u = sizeof(long);
    data = (char*) data + struct_sz_u;
  } else VM_ASSERT(false, "unhandled return type");
  // fprintf(stderr, "::");
  for (int i = 0; i < info->args_len; ++i) {
    Object *type = OBJ_OR_NULL(par_types_array->ptr[i]);
    Value val = load_arg(state->frame, INFO_ARGS_PTR(info)[i]);

    if (UNLIKELY(type == ffi->void_obj)) {
      VM_ASSERT(false, "void in parameter types??");
    }
    else if (type == ffi->float_obj) {
      // fprintf(stderr, "f");
      if (IS_FLOAT(val)) *(float*) data = AS_FLOAT(val);
      else if (IS_INT(val)) *(float*) data = AS_INT(val);
      else VM_ASSERT(false, "ffi float argument must be int or float");
      par_ptrs[i] = data;
      data = (char*) data + ((sizeof(float)>sizeof(long))?sizeof(float):sizeof(long));
    }
    else if (type == ffi->int_obj || type == ffi->uint_obj || type == ffi->int32_obj || type == ffi->uint32_obj
      || (sizeof(size_t) == 4 && type == ffi->size_t_obj)
    ) {
      // fprintf(stderr, "i32");
      Value val = load_arg(state->frame, INFO_ARGS_PTR(info)[i]);
      VM_ASSERT(IS_INT(val), "ffi int argument must be int");
      *(int*) data = AS_INT(val);
      par_ptrs[i] = data;
      data = (char*) data + sizeof(long);
    }
    else if (type == ffi->long_obj || type == ffi->ulong_obj) {
      // fprintf(stderr, "l");
      Value val = load_arg(state->frame, INFO_ARGS_PTR(info)[i]);
      VM_ASSERT(IS_INT(val), "ffi long argument must be int");
      *(long*) data = AS_INT(val);
      par_ptrs[i] = data;
      data = (char*) data + sizeof(long);
    }
    else if (type == ffi->int64_obj || type == ffi->uint64_obj
      || (sizeof(size_t) == 8 && type == ffi->size_t_obj)
    ) {
      // fprintf(stderr, "i64");
      Value val = load_arg(state->frame, INFO_ARGS_PTR(info)[i]);
      VM_ASSERT(IS_INT(val), "ffi (u)int64 argument must be int");
      *(int64_t*) data = AS_INT(val);
      par_ptrs[i] = data;
      data = (char*) data + sizeof(int64_t);
    }
    else if (type == ffi->double_obj) {
      // fprintf(stderr, "d");
      if (IS_FLOAT(val)) *(double*) data = AS_FLOAT(val);
      else if (IS_INT(val)) *(double*) data = AS_INT(val);
      else VM_ASSERT(false, "ffi double argument must be int or float");
      par_ptrs[i] = data;
      data = (char*) data + ((sizeof(double)>sizeof(long))?sizeof(double):sizeof(long));
    }
    else if (type == ffi->char_pointer_obj) {
      // fprintf(stderr, "pc");
      if (IS_NULL(val)) *(char**) data = NULL;
      else {
        StringObject *sobj = (StringObject*) obj_instance_of(OBJ_OR_NULL(val), string_base);
        VM_ASSERT(sobj, "ffi char* argument must be string");
        *(char**) data = sobj->value;
      }
      par_ptrs[i] = data;
      data = (char*) data + ((sizeof(char*)>sizeof(long))?sizeof(char*):sizeof(long));
    }
    else if (type == ffi->pointer_obj) {
      // fprintf(stderr, "p");
      if (IS_NULL(val)) {
        *(void**) data = NULL;
      } else {
        Object *obj = OBJ_OR_NULL(val);
        VM_ASSERT(obj && obj->parent == pointer_base, "ffi pointer argument %i must be pointer", i);
        PointerObject *pobj = (PointerObject*) obj;
        *(void**) data = pobj->ptr;
      }
      par_ptrs[i] = data;
      data = (char*) data + ((sizeof(void*)>sizeof(long))?sizeof(void*):sizeof(long));
    }
    else if (obj_instance_of(type, ffi->struct_obj)) {
      Value val = load_arg(state->frame, INFO_ARGS_PTR(info)[i]);
      VM_ASSERT(IS_OBJ(val), "ffi struct argument must be ffi struct object");
      Object *str_obj = AS_OBJ(val);
      // TODO same_type(a, b)
      // VM_ASSERT(obj_instance_of(str_obj, type), "ffi struct argument %i must match struct type", i);
      VM_ASSERT(obj_instance_of(str_obj, ffi->struct_obj), "ffi struct argument %i must be struct", i);

      Value sizeof_val = OBJECT_LOOKUP(str_obj, sizeof);
      VM_ASSERT(IS_INT(sizeof_val), "internal error: sizeof wrong type or undefined");
      int struct_size = AS_INT(sizeof_val);
      VM_ASSERT(struct_size >= 0 && struct_size < 16384, "scuse me wat r u doin");

      PointerObject *ptr_obj = (PointerObject*) obj_instance_of(OBJ_OR_NULL(OBJECT_LOOKUP(str_obj, pointer)), pointer_base);
      VM_ASSERT(ptr_obj, "struct's \"pointer\" not set, null or not a pointer");

      unsigned int step_size = struct_size;
      if (sizeof(long) > step_size) step_size = sizeof(long);

      memcpy(data, ptr_obj->ptr, struct_size);
      par_ptrs[i] = data;
      data = (char*) data + step_size;
    }
    else abort();
  }

  void (*sym_fn)() = *(void(**)())&sym_ptr;
  ffi_call(&ffihdl->cif, sym_fn, ret_ptr, par_ptrs);
  // fprintf(stderr, " -> ");
  if (ret_type == ffi->void_obj) {
    // fprintf(stderr, "v");
    vm_return(state, info, VNULL);
  } else if (ret_type == ffi->int8_obj) {
    // fprintf(stderr, "b");
    vm_return(state, info, INT2VAL(*(int8_t*) ret_ptr));
  } else if (ret_type == ffi->uint8_obj) {
    // fprintf(stderr, "ub");
    vm_return(state, info, INT2VAL(*(uint8_t*) ret_ptr));
  } else if (ret_type == ffi->int_obj) {
    // fprintf(stderr, "i");
    vm_return(state, info, INT2VAL(*(int*) ret_ptr));
  } else if (ret_type == ffi->uint_obj) {
    // fprintf(stderr, "u");
    vm_return(state, info, INT2VAL(*(unsigned int*) ret_ptr));
  } else if (ret_type == ffi->uint32_obj
    || (sizeof(size_t) == 4 && ret_type == ffi->size_t_obj)
  ) {
    // fprintf(stderr, "i32");
    vm_return(state, info, INT2VAL(*(uint32_t*) ret_ptr));
  } else if (ret_type == ffi->int64_obj || ret_type == ffi->uint64_obj
    || (sizeof(size_t) == 8 && ret_type == ffi->size_t_obj)
  ) {
    int64_t retval = *(int64_t*) ret_ptr;
    VM_ASSERT(retval >= INT32_MIN && retval <= INT32_MAX, "size of integer type exceeded on return");
    vm_return(state, info, INT2VAL((int32_t) retval));
  } else if (ret_type == ffi->char_pointer_obj) {
    // fprintf(stderr, "pc");
    vm_return(state, info, make_string(state, *(char**) ret_ptr, strlen(*(char**) ret_ptr)));
  } else if (ret_type == ffi->pointer_obj) {
    // fprintf(stderr, "p");
    if (*(void**) ret_ptr == NULL) {
      vm_return(state, info, VNULL);
    } else {
      vm_return(state, info, make_ffi_pointer(state, *(void**) ret_ptr));
    }
  } else if (ret_type == ffi->float_obj) {
    // fprintf(stderr, "f");
    vm_return(state, info, FLOAT2VAL(*(float*) ret_ptr));
  } else if (ret_type == ffi->double_obj) {
    // fprintf(stderr, "d");
    // TODO alloc_double?
    vm_return(state, info, FLOAT2VAL((float) *(double*) ret_ptr));
  } else if (obj_instance_of(ret_type, ffi->struct_obj)) {
    // validated above
    unsigned int struct_sz = AS_INT(OBJECT_LOOKUP(ret_type, sizeof));
    void *struct_data = malloc(struct_sz);
    memcpy(struct_data, ret_ptr, struct_sz);
    Object *struct_val_obj = AS_OBJ(make_object(state, ret_type, false));
    OBJECT_SET(state, struct_val_obj, pointer, make_ffi_pointer(state, struct_data));
    vm_return(state, info, OBJ2VAL(struct_val_obj));
  } else VM_ASSERT(false, "unknown return type");
  // fprintf(stderr, "\n");
}

static void ffi_call_fn_special_d_d(VMState *state, CallInfo *info) {
  Object *pointer_base = state->shared->vcache.pointer_base; (void) pointer_base;
  Object *function_base = state->shared->vcache.function_base;

  FFIFunctionObject *ffi_fn = (FFIFunctionObject*) obj_instance_of(AS_OBJ(load_arg(state->frame, info->fn)), function_base);

  Object *ffi_ptr_obj = ffi_fn->_ffi_pointer;
  Object *sym_ptr_obj = ffi_fn->_sym_pointer;
  assert(ffi_ptr_obj->parent == pointer_base && sym_ptr_obj->parent == pointer_base);
  PointerObject *ffi_ptr_obj_sub = (PointerObject*) ffi_ptr_obj;
  PointerObject *sym_ptr_obj_sub = (PointerObject*) sym_ptr_obj;
  FFIHandle *ffihdl = ffi_ptr_obj_sub->ptr;
  void *sym_ptr = sym_ptr_obj_sub->ptr;

  VM_ASSERT(info->args_len == 1, "FFI arity violated: expected 1, got %i", info->args_len);

  void **par_ptrs = alloca(sizeof(void*) * 1);
  void *data = alloca(sizeof(double));
  void *ret_ptr = alloca(sizeof(double));

  Value arg0 = load_arg(state->frame, INFO_ARGS_PTR(info)[0]);
  if (IS_FLOAT(arg0)) *(double*) data = AS_FLOAT(arg0);
  else if (IS_INT(arg0)) *(double*) data = AS_INT(arg0);
  else VM_ASSERT(false, "ffi double argument must be int or float");

  par_ptrs[0] = data;

  void (*sym_fn)() = *(void(**)())&sym_ptr;
  ffi_call(&ffihdl->cif, sym_fn, ret_ptr, par_ptrs);

  vm_return(state, info, FLOAT2VAL((float) *(double*) ret_ptr));
}

static void ffi_call_fn_special_fx_v(VMState * __restrict__ state, CallInfo * __restrict__ info) __attribute__ ((hot));
static void ffi_call_fn_special_fx_v(VMState * __restrict__ state, CallInfo * __restrict__ info) {
  Object *pointer_base = state->shared->vcache.pointer_base; (void) pointer_base;
  Object *function_base = state->shared->vcache.function_base;

  FFIFunctionObject *ffi_fn = (FFIFunctionObject*) obj_instance_of(AS_OBJ(load_arg(state->frame, info->fn)), function_base);

  ArrayObject *par_types_array = ffi_fn->par_types_array;
  Object *ffi_ptr_obj = ffi_fn->_ffi_pointer;
  Object *sym_ptr_obj = ffi_fn->_sym_pointer;
  assert(ffi_ptr_obj->parent == pointer_base && sym_ptr_obj->parent == pointer_base);
  PointerObject *ffi_ptr_obj_sub = (PointerObject*) ffi_ptr_obj;
  PointerObject *sym_ptr_obj_sub = (PointerObject*) sym_ptr_obj;
  FFIHandle *ffihdl = ffi_ptr_obj_sub->ptr;
  void *sym_ptr = sym_ptr_obj_sub->ptr;

  VM_ASSERT(info->args_len == par_types_array->length, "FFI arity violated: expected %i, got %i", par_types_array->length, info->args_len);

  void **par_ptrs = alloca(sizeof(void*) * 3);
  void *data = alloca(sizeof(float) * 3);
  void *ret_ptr = NULL;

  for (int i = 0; i < info->args_len; ++i) {
    Value arg = load_arg(state->frame, INFO_ARGS_PTR(info)[i]);
    if (IS_FLOAT(arg)) *(float*) data = AS_FLOAT(arg);
    else if (IS_INT(arg)) *(float*) data = AS_INT(arg);
    else VM_ASSERT(false, "ffi float argument %i must be int or float, not %s", i, get_type_info(state, arg));
    par_ptrs[i] = data;
    data = (void*) ((float*) data + 1);
  }

  void (*sym_fn)() = *(void(**)())&sym_ptr;
  ffi_call(&ffihdl->cif, sym_fn, ret_ptr, par_ptrs);
  vm_return(state, info, VNULL);
}

VMFunctionPointer ffi_get_specialized_call_fn(FFIObject *ffi, Object *ret_type, ArrayObject *par_types) {
  if (ret_type == ffi->double_obj
    && par_types->length == 1 && AS_OBJ(par_types->ptr[0]) == ffi->double_obj)
  {
    return ffi_call_fn_special_d_d;
  }
  if (ret_type == ffi->void_obj) {
    bool all_float = true;
    for (int i = 0; i < par_types->length; i++) {
      if (AS_OBJ(par_types->ptr[i]) != ffi->float_obj) {
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

static void ffi_sym_fn(VMState *state, CallInfo *info) {
  VM_ASSERT(info->args_len == 3, "wrong arity: expected 3, got %i", info->args_len);
  Object *array_base = state->shared->vcache.array_base;
  Object *string_base = state->shared->vcache.string_base;
  Object *pointer_base = state->shared->vcache.pointer_base;
  Object *ffi = state->shared->vcache.ffi_obj;
  FFIObject *ffi_obj = (FFIObject*) ffi;
  Object *handle_base = AS_OBJ(OBJECT_LOOKUP(ffi, handle));
  Object *type_base = AS_OBJ(OBJECT_LOOKUP(ffi, type));

  Object *handle_obj = obj_instance_of(OBJ_OR_NULL(load_arg(state->frame, info->this_arg)), handle_base);
  VM_ASSERT(handle_obj, "ffi sym called on bad object");
  Object *handle_ptr_obj = OBJ_OR_NULL(OBJECT_LOOKUP(handle_obj, pointer));
  VM_ASSERT(handle_ptr_obj && handle_ptr_obj->parent == pointer_base, "sym handle must be pointer");
  PointerObject *handle_ptr = (PointerObject*) handle_ptr_obj;
  void *handle = handle_ptr->ptr;

  StringObject *fn_name_obj = (StringObject*) obj_instance_of(OBJ_OR_NULL(load_arg(state->frame, INFO_ARGS_PTR(info)[0])), string_base);
  VM_ASSERT(fn_name_obj, "symbol name must be string");

  void *fnptr = my_dlsym(handle, fn_name_obj->value);
  const char *error = my_dlerror(handle);
  // VM_ASSERT(!error, "dlsym failed: %s", error);
  if (error) { vm_return(state, info, VNULL); return; }

  Object *ret_type = OBJ_OR_NULL(load_arg(state->frame, INFO_ARGS_PTR(info)[1]));
  if (obj_instance_of(ret_type, ffi_obj->struct_obj)) { } // keep ret_type the deepest obj
  else ret_type = obj_instance_of(ret_type, type_base); // else bring down to right beneath type_base so we can ==
  VM_ASSERT(ret_type, "return type must be ffi.type!");

  ArrayObject *par_types = (ArrayObject*) obj_instance_of(OBJ_OR_NULL(load_arg(state->frame, INFO_ARGS_PTR(info)[2])), array_base);
  VM_ASSERT(par_types, "parameter type must be array");

  ffi_type *ffi_ret_type = type_to_ffi_ptr(state, ffi, ret_type);
  FFIHandle *ffihdl = malloc(sizeof(FFIHandle) + sizeof(ffi_type*) * par_types->length);

  for (int i = 0; i < par_types->length; ++i) {
    Object *sub_type = OBJ_OR_NULL(par_types->ptr[i]);
    if (obj_instance_of(sub_type, ffi_obj->struct_obj)) { } // keep sub_type the deepest obj
    else sub_type = obj_instance_of(sub_type, type_base); // else bring down to right beneath type_base so we can ==
    VM_ASSERT(sub_type, "parameter type %i must be ffi.type!", i);

    ((ffi_type**)(ffihdl + 1))[i] = type_to_ffi_ptr(state, ffi, sub_type);
  }

  ffi_status status = ffi_prep_cif(&ffihdl->cif, FFI_DEFAULT_ABI, par_types->length, ffi_ret_type, (ffi_type**)(ffihdl + 1));
  VM_ASSERT(status == FFI_OK, "FFI error: %i", status);

  VMFunctionPointer ffi_spec_fn = ffi_get_specialized_call_fn((FFIObject*) ffi, ret_type, par_types);

  Object *fn_obj = AS_OBJ(make_fn_custom(state, ffi_spec_fn, NULL, sizeof(FFIFunctionObject), true));
  FFIFunctionObject *ffi_fn = (FFIFunctionObject*) fn_obj;
  Object *_sym_pointer = AS_OBJ(make_ptr(state, fnptr));
  Object *_ffi_pointer = AS_OBJ(make_ptr(state, (void*) ffihdl));

  fn_obj->flags |= OBJ_FROZEN;
  OBJECT_SET(state, fn_obj, return_type, OBJ2VAL(ret_type));
  OBJECT_SET(state, fn_obj, parameter_types, load_arg(state->frame, INFO_ARGS_PTR(info)[2]));
  OBJECT_SET(state, fn_obj, _sym_pointer, OBJ2VAL(_sym_pointer));
  OBJECT_SET(state, fn_obj, _ffi_pointer, OBJ2VAL(_ffi_pointer));
  ffi_fn->return_type = ret_type;
  // use the array here, since we don't care about any subtypes
  ffi_fn->par_types_array = par_types;
  ffi_fn->_sym_pointer = _sym_pointer;
  ffi_fn->_ffi_pointer = _ffi_pointer;
  ffi_fn->par_len_sum_precomp = ffi_par_len(state, ret_type, par_types, (FFIObject*) ffi);

  vm_return(state, info, OBJ2VAL(fn_obj));
}

static void malloc_fn(VMState *state, CallInfo *info) {
  VM_ASSERT(info->args_len == 1, "wrong arity: expected 1, got %i", info->args_len);
  VM_ASSERT(IS_INT(load_arg(state->frame, INFO_ARGS_PTR(info)[0])), "malloc expected int");
  VM_ASSERT(AS_INT(load_arg(state->frame, INFO_ARGS_PTR(info)[0])) >= 0, "malloc expected positive number");
  void *res = malloc(AS_INT(load_arg(state->frame, INFO_ARGS_PTR(info)[0])));
  VM_ASSERT(res, "memory allocation failed");
  vm_return(state, info, make_ffi_pointer(state, res));
}

void ffi_setup_root(VMState *state, Object *root) {
  FFIObject *ffi = (FFIObject*) alloc_object_internal(state, sizeof(FFIObject), false);
  Object *ffi_obj = (Object*) ffi;
  ffi_obj->flags |= OBJ_FROZEN;

  OBJECT_SET(state, (Object*) ffi_obj, open, make_fn(state, ffi_open_fn));
  Object *type_obj = AS_OBJ(make_object(state, NULL, false));
  OBJECT_SET(state, (Object*) ffi_obj, type, OBJ2VAL(type_obj));

#define DEFINE_TYPE(NAME, T) ffi->NAME ## _obj = AS_OBJ(make_object(state, type_obj, false)); \
  ffi->NAME ## _obj->flags |= OBJ_NOINHERIT; \
  OBJECT_SET(state, ffi->NAME ## _obj, sizeof, INT2VAL(sizeof(T))); \
  OBJECT_SET(state, ffi->NAME ## _obj, c_type, make_string(state, #T, strlen(#T))); \
  OBJECT_SET(state, ffi_obj, NAME, OBJ2VAL(ffi->NAME ## _obj))
  ffi->void_obj = AS_OBJ(make_object(state, type_obj, false));
  ffi->void_obj->flags |= OBJ_NOINHERIT;
  OBJECT_SET(state, ffi_obj, void, OBJ2VAL(ffi->void_obj));
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
  if (sizeof(size_t) == sizeof(uint32_t)) {
    DEFINE_TYPE(size_t, uint32_t);
  } else if (sizeof(size_t) == sizeof(uint64_t)) {
    DEFINE_TYPE(size_t, uint64_t);
  } else abort(); // 128-bit? 16-bit?
#undef DEFINE_TYPE

  Object *handle_obj = AS_OBJ(make_object(state, NULL, false));
  OBJECT_SET(state, ffi_obj, handle, OBJ2VAL(handle_obj));
  OBJECT_SET(state, handle_obj, pointer, VNULL);
  OBJECT_SET(state, handle_obj, sym, make_fn(state, ffi_sym_fn));

  Object *struct_obj = AS_OBJ(make_object(state, type_obj, false));
  ffi->struct_obj = struct_obj;
  OBJECT_SET(state, ffi_obj, struct, OBJ2VAL(struct_obj));
  OBJECT_SET(state, struct_obj, complete, BOOL2VAL(false));
  OBJECT_SET(state, struct_obj, pointer, VNULL);
  OBJECT_SET(state, struct_obj, members, VNULL);

  Object *lib_map = AS_OBJ(make_object(state, NULL, false));
#ifdef _WIN32
  FastKey fkey;
#define OBJECT_SET_LIBMAP(STR,V) fkey=prepare_key(STR,strlen(STR));object_set(state,libmap,&fkey,V)
  OBJECT_SET_LIBMAP("libGL.so", make_string_static(state, "opengl32.dll"));
  OBJECT_SET_LIBMAP("libglfw.so", make_string_static(state, "glfw3.dll"));
  OBJECT_SET_LIBMAP("libglfwq.so", make_string_static(state, "glfwq.dll"));
  OBJECT_SET_LIBMAP("libcsfml-audio.so", make_string_static(state, "csfml-audio-2.dll"));
  OBJECT_SET_LIBMAP("libcsfml-system.so", make_string_static(state, "csfml-system-2.dll"));
  OBJECT_SET_LIBMAP("libcairo.so", make_string_static(state, "libcairo-2.dll"));
  OBJECT_SET_LIBMAP("libc.so.6", make_string_static(state, "msvcrt.dll"));
  OBJECT_SET_LIBMAP("libSOIL.so", make_string_static(state, "libSOIL.dll"));
  Value *cairo_list = malloc(sizeof(Value) * 3);
  cairo_list[0] = make_string_static(state, "libpangocairo-1.0-0.dll");
  cairo_list[1] = make_string_static(state, "libpango-1.0-0.dll");
  cairo_list[2] = make_string_static(state, "libgobject-2.0-0.dll");
  OBJECT_SET_LIBMAP("libpangocairo-1.0.so", make_array(state, cairo_list, 3, true));
#undef OBJECT_SET_LIBMAP
#endif
  OBJECT_SET(state, ffi_obj, library_map, OBJ2VAL(lib_map));

  OBJECT_SET(state, root, ffi, OBJ2VAL(ffi_obj));

  OBJECT_SET(state, root, malloc, make_fn(state, malloc_fn));

  state->shared->vcache.ffi_obj = ffi_obj;
}
