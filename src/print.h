#ifndef PRINT_H
#define PRINT_H

#include <stdio.h>
#include "object.h"

void print_recursive(VMState *state, FILE *fh, Object *obj, bool allow_tostring);

#endif
