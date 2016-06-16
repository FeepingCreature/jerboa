#ifndef LANGUAGE_H
#define LANGUAGE_H

#include "vm/instr.h"
#include "parser.h"

ParseResult parse_module(char **textp, UserFunction **uf_p);

#endif
