#ifndef PARSER_H
#define PARSER_H

#include <stdbool.h>

void eat_filler(char **textp);

bool eat_string(char **textp, char *keyword);

bool eat_keyword(char **textp, char *keyword);

char *parse_identifier_all(char **textp);

char *parse_identifier(char **textp);

bool parse_int(char **textp, int *outp);

bool parse_float(char **textp, float *outp);

bool parse_string(char **textp, char **outp);

void log_parser_error(char *location, char *format, ...);

#endif
