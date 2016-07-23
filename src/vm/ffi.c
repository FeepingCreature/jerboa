#include "vm/ffi.h"
#include "object.h"

#include <dlfcn.h>
#include <stdio.h>
#include <stdint.h>
#include <alloca.h>

typedef struct {
  ffi_cif cif;
} FFIHandle;


static void ffi_open_fn(VMState *state, CallInfo *info) {
  VM_ASSERT(info->args_len == 1, "wrong arity: expected 1, got %i", info->args_len);
  Object *root = state->root;
  Object *string_base = state->shared->vcache.string_base;
  Object *ffi = AS_OBJ(OBJECT_LOOKUP_STRING(root, "ffi", NULL));
  Object *handle_base = AS_OBJ(OBJECT_LOOKUP_STRING(ffi, "handle", NULL));
  
  StringObject *sarg = (StringObject*) obj_instance_of(OBJ_OR_NULL(load_arg(state->frame, INFO_ARGS_PTR(info)[0])), string_base);
  VM_ASSERT(sarg, "argument to ffi.open must be string!");
  
  char *file = sarg->value;
  void *dlptr = dlopen(file, RTLD_LAZY);
  VM_ASSERT(dlptr, "dlopen failed: %s", dlerror());
  
  Object *handle_obj = AS_OBJ(make_object(state, handle_base));
  handle_obj->flags |= OBJ_FROZEN;
  object_set(state, handle_obj, "pointer", make_ptr(state, dlptr));
  *state->frame->target_slot = OBJ2VAL(handle_obj);
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

static Value make_ffi_pointer(VMState *state, void *ptr);

static void ffi_ptr_dereference(VMState *state, CallInfo *info) {
  VM_ASSERT(info->args_len == 2, "wrong arity: expected 2, got %i", info->args_len);
  Object *root = state->root;
  Object *pointer_base = state->shared->vcache.pointer_base;
  
  FFIObject *ffi = (FFIObject*) AS_OBJ(OBJECT_LOOKUP_STRING(root, "ffi", NULL));
  Object *ffi_type = AS_OBJ(OBJECT_LOOKUP_STRING((Object*) ffi, "type", NULL));
  Object *ffi_short = ffi->short_obj;
  Object *ffi_int = ffi->int_obj;
  Object *ffi_ushort = ffi->ushort_obj;
  Object *ffi_int8 = ffi->int8_obj;
  Object *ffi_uint8 = ffi->uint8_obj;
  // Object *ffi_uint32 = ffi->uint32_obj;
  Object *ffi_pointer = ffi->pointer_obj;
  Object *ffi_charptr = ffi->char_pointer_obj;
  
  Object *thisptr = AS_OBJ(load_arg(state->frame, info->this_arg));
  VM_ASSERT(thisptr->parent == pointer_base, "internal error");
  PointerObject *thisptr_obj = (PointerObject*) thisptr;
  
  Object *ffi_type_obj = obj_instance_of(OBJ_OR_NULL(load_arg(state->frame, INFO_ARGS_PTR(info)[0])), ffi_type);
  Value offs_val = load_arg(state->frame, INFO_ARGS_PTR(info)[1]);
  VM_ASSERT(IS_INT(offs_val), "offset must be integer");
  int offs = AS_INT(offs_val);
  assert(ffi_type_obj);
  char *offset_ptr = (char*) thisptr_obj->ptr + offs;
  if (ffi_type_obj == ffi_short) {
    short s = *(short*) offset_ptr;
    *state->frame->target_slot = INT2VAL(s);
  } else if (ffi_type_obj == ffi_ushort) {
    unsigned short us = *(unsigned short*) offset_ptr;
    *state->frame->target_slot = INT2VAL(us);
  } else if (ffi_type_obj == ffi_int) {
    int i = *(int*) offset_ptr;
    *state->frame->target_slot = INT2VAL(i);
  } else if (ffi_type_obj == ffi_uint8) {
    uint8_t u8 = *(uint8_t*) offset_ptr;
    *state->frame->target_slot = INT2VAL(u8);
  } else if (ffi_type_obj == ffi_int8) {
    int8_t i8 = *(int8_t*) offset_ptr;
    *state->frame->target_slot = INT2VAL(i8);
  } else if (ffi_type_obj == ffi_pointer) {
    void *ptr = *(void**) offset_ptr;
    *state->frame->target_slot = make_ffi_pointer(state, ptr);
  } else if (ffi_type_obj == ffi_charptr) {
    char *ptr = *(char**) offset_ptr;
    *state->frame->target_slot = make_string_foreign(state, ptr);
  } else assert("TODO" && false);
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
    Object *c_type_obj = AS_OBJ(OBJECT_LOOKUP_STRING(type, "c_type", NULL));
    StringObject *c_type = (StringObject*) obj_instance_of_or_equal(c_type_obj, string_base);
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
  } else if (type == ffi->uint_obj) {
    unsigned int i = *(unsigned int*) ptr;
    return INT2VAL(i);
  } else {
    Object *c_type_obj = OBJ_OR_NULL(OBJECT_LOOKUP_STRING(type, "c_type", NULL));
    StringObject *c_type = (StringObject*) obj_instance_of_or_equal(c_type_obj, string_base);
    VM_ASSERT(c_type, "internal type error") VNULL;
    VM_ASSERT(false, "unhandled pointer read type: %s", c_type->value) VNULL;
  }
}

static void ffi_ptr_index_fn(VMState *state, CallInfo *info) {
  VM_ASSERT(info->args_len == 1, "wrong arity: expected 1, got %i", info->args_len);
  Object *pointer_base = state->shared->vcache.pointer_base;
  Object *thisptr = OBJ_OR_NULL(load_arg(state->frame, info->this_arg));
  
  VM_ASSERT(thisptr && thisptr->parent == pointer_base, "invalid pointer index on non-pointer object");
  PointerObject *thisptr_obj = (PointerObject*) thisptr;
  
  Object *ffi_type_obj = OBJ_OR_NULL(OBJECT_LOOKUP_STRING(thisptr, "target_type", NULL));
  VM_ASSERT(ffi_type_obj, "cannot index read on untyped pointer!");
  
  Value offs_val = load_arg(state->frame, INFO_ARGS_PTR(info)[0]);
  VM_ASSERT(IS_INT(offs_val), "offset must be integer");
  int offs = AS_INT(offs_val);
  
  Value sizeof_val = OBJECT_LOOKUP_STRING(ffi_type_obj, "sizeof", NULL);
  VM_ASSERT(IS_INT(sizeof_val), "internal error: sizeof wrong type or undefined");
  int elemsize = AS_INT(sizeof_val);
  
  char *offset_ptr = (char*) thisptr_obj->ptr + elemsize * offs;
  
  Value res = ffi_pointer_read(state, ffi_type_obj, (void*) offset_ptr);
  // sometimes called naked
  if (state->frame) *state->frame->target_slot = res;
  else state->exit_value = res;
}

static void ffi_ptr_index_assign_fn(VMState *state, CallInfo *info) {
  VM_ASSERT(info->args_len == 2, "wrong arity: expected 2, got %i", info->args_len);
  Object *pointer_base = state->shared->vcache.pointer_base;
  Object *thisptr = OBJ_OR_NULL(load_arg(state->frame, info->this_arg));
  
  VM_ASSERT(thisptr && thisptr->parent == pointer_base, "invalid pointer index write on non-pointer object");
  PointerObject *thisptr_obj = (PointerObject*) thisptr;
  
  Object *ffi_type_obj = AS_OBJ(OBJECT_LOOKUP_STRING(thisptr, "target_type", NULL));
  VM_ASSERT(ffi_type_obj, "cannot assign index on untyped pointer!");
  
  Value offs_val = load_arg(state->frame, INFO_ARGS_PTR(info)[0]);
  VM_ASSERT(IS_INT(offs_val), "offset must be integer");
  int offs = AS_INT(offs_val);
  
  Value sizeof_val = OBJECT_LOOKUP_STRING(ffi_type_obj, "sizeof", NULL);
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
  
  Value offset_val = load_arg(state->frame, INFO_ARGS_PTR(info)[0]);
  VM_ASSERT(IS_INT(offset_val), "offset must be integer");
  int offset = AS_INT(offset_val);
  
  *state->frame->target_slot = make_ffi_pointer(state, (void*) ((char*)ptr + offset));
}

static Value make_ffi_pointer(VMState *state, void *ptr) {
  Object *ptr_obj = AS_OBJ(make_ptr(state, ptr));
  object_set(state, ptr_obj, "dereference", make_fn(state, ffi_ptr_dereference));
  object_set(state, ptr_obj, "+", make_fn(state, ffi_ptr_add));
  object_set(state, ptr_obj, "target_type", VNULL);
  object_set(state, ptr_obj, "[]", make_fn(state, ffi_ptr_index_fn));
  object_set(state, ptr_obj, "[]=", make_fn(state, ffi_ptr_index_assign_fn));
  return OBJ2VAL(ptr_obj);
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
    else if (type == ffi->int_obj || type == ffi->uint_obj || type == ffi->int32_obj || type == ffi->uint32_obj) {
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
    else if (type == ffi->int64_obj || type == ffi->uint64_obj) {
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
    else abort();
  }
  
  void (*sym_fn)() = *(void(**)())&sym_ptr;
  ffi_call(&ffihdl->cif, sym_fn, ret_ptr, par_ptrs);
  // fprintf(stderr, " -> ");
  if (ret_type == ffi->void_obj) {
    // fprintf(stderr, "v");
    *state->frame->target_slot = VNULL;
  } else if (ret_type == ffi->int_obj) {
    // fprintf(stderr, "i");
    *state->frame->target_slot = INT2VAL(*(int*) ret_ptr);
  } else if (ret_type == ffi->uint_obj) {
    // fprintf(stderr, "u");
    *state->frame->target_slot = INT2VAL(*(unsigned int*) ret_ptr);
  } else if (ret_type == ffi->uint32_obj) {
    // fprintf(stderr, "i32");
    *state->frame->target_slot = INT2VAL(*(uint32_t*) ret_ptr);
  } else if (ret_type == ffi->char_pointer_obj) {
    // fprintf(stderr, "pc");
    *state->frame->target_slot = make_string(state, *(char**) ret_ptr, strlen(*(char**) ret_ptr));
  } else if (ret_type == ffi->pointer_obj) {
    // fprintf(stderr, "p");
    *state->frame->target_slot = make_ffi_pointer(state, *(void**) ret_ptr);
  } else if (ret_type == ffi->float_obj) {
    // fprintf(stderr, "f");
    *state->frame->target_slot = FLOAT2VAL(*(float*) ret_ptr);
  } else if (ret_type == ffi->double_obj) {
    // fprintf(stderr, "d");
    // TODO alloc_double?
    *state->frame->target_slot = FLOAT2VAL((float) *(double*) ret_ptr);
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
  
  *state->frame->target_slot = FLOAT2VAL((float) *(double*) ret_ptr);
}

static void ffi_call_fn_special_fx_v(VMState *state, CallInfo *info) {
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
  *state->frame->target_slot = VNULL;
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
  Object *handle_base = AS_OBJ(OBJECT_LOOKUP_STRING(ffi, "handle", NULL));
  Object *type_base = AS_OBJ(OBJECT_LOOKUP_STRING(ffi, "type", NULL));
  
  Object *handle_obj = obj_instance_of(OBJ_OR_NULL(load_arg(state->frame, info->this_arg)), handle_base);
  VM_ASSERT(handle_obj, "ffi sym called on bad object");
  Object *handle_ptr_obj = OBJ_OR_NULL(OBJECT_LOOKUP_STRING(handle_obj, "pointer", NULL));
  VM_ASSERT(handle_ptr_obj && handle_ptr_obj->parent == pointer_base, "sym handle must be pointer");
  PointerObject *handle_ptr = (PointerObject*) handle_ptr_obj;
  void *handle = handle_ptr->ptr;
  
  StringObject *fn_name_obj = (StringObject*) obj_instance_of(OBJ_OR_NULL(load_arg(state->frame, INFO_ARGS_PTR(info)[0])), string_base);
  VM_ASSERT(fn_name_obj, "symbol name must be string");
  
  void *fnptr = dlsym(handle, fn_name_obj->value);
  char *error = dlerror();
  // VM_ASSERT(!error, "dlsym failed: %s", error);
  if (error) { *state->frame->target_slot = VNULL; return; }
  
  Object *ret_type = obj_instance_of(OBJ_OR_NULL(load_arg(state->frame, INFO_ARGS_PTR(info)[1])), type_base);
  VM_ASSERT(ret_type, "return type must be ffi.type!");
  
  ArrayObject *par_types = (ArrayObject*) obj_instance_of(OBJ_OR_NULL(load_arg(state->frame, INFO_ARGS_PTR(info)[2])), array_base);
  VM_ASSERT(par_types, "parameter type must be array");
  
  ffi_type *ffi_ret_type = type_to_ffi_ptr(ffi, ret_type);
  FFIHandle *ffihdl = malloc(sizeof(FFIHandle) + sizeof(ffi_type*) * par_types->length);
  
  for (int i = 0; i < par_types->length; ++i) {
    Object *sub_type = obj_instance_of(OBJ_OR_NULL(par_types->ptr[i]), type_base);
    VM_ASSERT(sub_type, "parameter type %i must be ffi.type!", i);
    ((ffi_type**)(ffihdl + 1))[i] = type_to_ffi_ptr(ffi, sub_type);
  }
  
  ffi_status status = ffi_prep_cif(&ffihdl->cif, FFI_DEFAULT_ABI, par_types->length, ffi_ret_type, (ffi_type**)(ffihdl + 1));
  VM_ASSERT(status == FFI_OK, "FFI error: %i", status);
  
  VMFunctionPointer ffi_spec_fn = ffi_get_specialized_call_fn((FFIObject*) ffi, ret_type, par_types);
  
  Object *fn_obj = AS_OBJ(make_fn_custom(state, ffi_spec_fn, sizeof(FFIFunctionObject)));
  FFIFunctionObject *ffi_fn = (FFIFunctionObject*) fn_obj;
  Object *_sym_pointer = AS_OBJ(make_ptr(state, fnptr));
  Object *_ffi_pointer = AS_OBJ(make_ptr(state, (void*) ffihdl));
  
  fn_obj->flags |= OBJ_FROZEN;
  object_set(state, fn_obj, "return_type", OBJ2VAL(ret_type));
  object_set(state, fn_obj, "parameter_types", load_arg(state->frame, INFO_ARGS_PTR(info)[2]));
  object_set(state, fn_obj, "_sym_pointer", OBJ2VAL(_sym_pointer));
  object_set(state, fn_obj, "_ffi_pointer", OBJ2VAL(_ffi_pointer));
  ffi_fn->return_type = ret_type;
  // use the array here, since we don't care about any subtypes
  ffi_fn->par_types_array = par_types;
  ffi_fn->_sym_pointer = _sym_pointer;
  ffi_fn->_ffi_pointer = _ffi_pointer;
  ffi_fn->par_len_sum_precomp = ffi_par_len(ret_type, par_types, (FFIObject*) ffi);
  
  *state->frame->target_slot = OBJ2VAL(fn_obj);
}

static void malloc_fn(VMState *state, CallInfo *info) {
  VM_ASSERT(info->args_len == 1, "wrong arity: expected 1, got %i", info->args_len);
  VM_ASSERT(IS_INT(load_arg(state->frame, INFO_ARGS_PTR(info)[0])), "malloc expected int");
  VM_ASSERT(AS_INT(load_arg(state->frame, INFO_ARGS_PTR(info)[0])) >= 0, "malloc expected positive number");
  void *res = malloc(AS_INT(load_arg(state->frame, INFO_ARGS_PTR(info)[0])));
  VM_ASSERT(res, "memory allocation failed");
  *state->frame->target_slot = make_ffi_pointer(state, res);
}

void ffi_setup_root(VMState *state, Object *root) {
  FFIObject *ffi = (FFIObject*) alloc_object_internal(state, sizeof(FFIObject));
  Object *ffi_obj = (Object*) ffi;
  ffi_obj->flags |= OBJ_FROZEN;
  
  object_set(state, (Object*) ffi_obj, "open", make_fn(state, ffi_open_fn));
  Object *type_obj = AS_OBJ(make_object(state, NULL));
  object_set(state, (Object*) ffi_obj, "type", OBJ2VAL(type_obj));
  
#define DEFINE_TYPE(NAME, T) ffi->NAME ## _obj = AS_OBJ(make_object(state, type_obj)); \
  ffi->NAME ## _obj->flags |= OBJ_NOINHERIT; \
  object_set(state, ffi->NAME ## _obj, "sizeof", INT2VAL(sizeof(T))); \
  object_set(state, ffi->NAME ## _obj, "c_type", make_string(state, #T, strlen(#T))); \
  object_set(state, ffi_obj, #NAME, OBJ2VAL(ffi->NAME ## _obj))
  ffi->void_obj = AS_OBJ(make_object(state, type_obj));
  ffi->void_obj->flags |= OBJ_NOINHERIT;
  object_set(state, ffi_obj, "void", OBJ2VAL(ffi->void_obj));
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

  Object *handle_obj = AS_OBJ(make_object(state, NULL));
  object_set(state, ffi_obj, "handle", OBJ2VAL(handle_obj));
  object_set(state, handle_obj, "pointer", VNULL);
  object_set(state, handle_obj, "sym", make_fn(state, ffi_sym_fn));
  
  object_set(state, root, "ffi", OBJ2VAL(ffi_obj));
  
  object_set(state, root, "malloc", make_fn(state, malloc_fn));
  
  state->shared->vcache.ffi_obj = ffi_obj;
}
