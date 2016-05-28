#ifndef VM_CALL_H
#define VM_CALL_H

#include "object.h"

Object *call_function(Object *context, UserFunction *fn, Object **args_ptr, int args_len);

Object *user_function_handler(Object *context, Object *fn, Object **args_ptr, int args_len);

Object *alloc_user_fn(Object *context, UserFunction *fn);

#endif
