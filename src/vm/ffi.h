#ifndef VM_FFI_H
#define VM_FFI_H

#include <ffi.h>

#include "vm/vm.h"

void ffi_setup_root(VMState *state, Object *root);

#endif
