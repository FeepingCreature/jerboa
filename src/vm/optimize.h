#ifndef OPTIMIZE_H
#define OPTIMIZE_H

#include "object.h"

UserFunction *optimize(UserFunction *uf);

UserFunction *optimize_runtime(VMState *state, UserFunction *uf, Object *context);

#endif
