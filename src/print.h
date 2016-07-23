#ifndef PRINT_H
#define PRINT_H

#include <stdio.h>
#include "object.h"

void print_recursive(VMState *state, FILE *fh, Value val, bool allow_tostring);

#endif
