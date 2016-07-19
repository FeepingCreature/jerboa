#ifndef VM_DUMP_H
#define VM_DUMP_H

#include "object.h"

void dump_instr(VMState *, Instr **instr);

void dump_fn(VMState *, UserFunction *fn);

#endif
