#ifndef VM_DUMP_H
#define VM_DUMP_H

#include "object.h"

void dump_instr(Instr *instr);

void dump_fn(UserFunction *fn);

#endif
