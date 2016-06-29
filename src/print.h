#ifndef PRINT_H
#define PRINT_H

#include "object.h"

void print_recursive(VMState *state, Object *obj, bool allow_tostring);

#endif
