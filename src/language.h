#ifndef JERBOA_LANGUAGE_H
#define JERBOA_LANGUAGE_H

#include "vm/instr.h"
#include "rdparse/parser.h"

ParseResult parse_module(char **textp, UserFunction **uf_p);

#endif
