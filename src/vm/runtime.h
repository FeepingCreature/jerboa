#ifndef VM_RUNTIME_H
#define VM_RUNTIME_H

#include "object.h"

Object *create_root(VMState *state);

typedef enum {
  CMP_EQ,
  CMP_LT,
  CMP_GT,
  CMP_LE,
  CMP_GE
} CompareOp;

#endif
