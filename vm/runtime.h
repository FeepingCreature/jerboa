#ifndef VM_RUNTIME_H
#define VM_RUNTIME_H

#include "object.h"

Object *equals(Object *context, Object *fn, Object **args_ptr, int args_len);

Object *add(Object *context, Object *fn, Object **args_ptr, int args_len);

Object *sub(Object *context, Object *fn, Object **args_ptr, int args_len);

Object *mul(Object *context, Object *fn, Object **args_ptr, int args_len);

Object *create_root();

#endif
