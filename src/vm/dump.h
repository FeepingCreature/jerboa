#ifndef JERBOA_VM_DUMP_H
#define JERBOA_VM_DUMP_H

#include "object.h"

void dump_instr(VMState *, Instr **instr);

void dump_fn(VMState *, UserFunction *fn);

#endif
