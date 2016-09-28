#ifndef JERBOA_VM_OPTIMIZE_H
#define JERBOA_VM_OPTIMIZE_H

#include "object.h"

UserFunction *optimize(UserFunction *uf);

UserFunction *optimize_runtime(VMState *state, UserFunction *uf, Object *context);

#endif
