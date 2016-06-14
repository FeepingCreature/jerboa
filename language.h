#ifndef LANGUAGE_H
#define LANGUAGE_H

#include "vm/instr.h"

typedef enum {
  PARSE_NONE,
  PARSE_ERROR,
  PARSE_OK
} ParseResult;

ParseResult parse_module(char **textp, UserFunction **uf_p);

#endif
