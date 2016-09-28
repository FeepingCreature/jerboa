#ifndef JERBOA_PARSER_H
#define JERBOA_PARSER_H

#include <stdbool.h>

#include "util.h"

typedef enum {
  PARSE_NONE,
  PARSE_ERROR,
  PARSE_OK
} ParseResult;

void eat_filler(char **textp);

bool eat_string(char **textp, char *keyword);

bool eat_keyword(char **textp, char *keyword);

char *parse_identifier_all(char **textp);

char *parse_identifier(char **textp);

bool parse_int(char **textp, int *outp);

bool parse_float(char **textp, float *outp);

ParseResult parse_string(char **textp, char **outp);

void log_parser_error(char *location, char *format, ...);

#endif
