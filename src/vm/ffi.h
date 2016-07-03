#ifndef VM_FFI_H
#define VM_FFI_H

#include <ffi.h>

#include "vm/vm.h"

typedef struct {
  Object base;
  Object *void_obj,
    *sint_obj, *uint_obj,
    *long_obj, *ulong_obj,
    *float_obj, *double_obj,
    *int8_obj, *int16_obj, *int32_obj, *int64_obj,
    *uint8_obj, *uint16_obj, *uint32_obj, *uint64_obj,
    *pointer_obj, *char_pointer_obj;
} FFIObject;

void ffi_setup_root(VMState *state, Object *root);

#endif
