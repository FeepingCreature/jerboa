#ifndef PARSER_H
#define PARSER_H

#include "vm/builder.h"

void eat_filler(char **textp);

bool eat_string(char **textp, char *keyword);

bool eat_keyword(char **textp, char *keyword);

char *parse_identifier_all(char **textp);

char *parse_identifier(char **textp);

bool parse_int(char **textp, int *outp);

void parser_error(char *location, char *format, ...) __attribute__ ((noreturn));

int parse_expr(char **textp, FunctionBuilder *builder, int level);

int parse_expr_base(char **textp, FunctionBuilder *builder);

bool parse_cont_call(char **textp, FunctionBuilder *builder, int *expr);

int parse_expr_base_tail(char **textp, FunctionBuilder *builder);

int parse_expr(char **textp, FunctionBuilder *builder, int level);

void parse_block(char **textp, FunctionBuilder *builder);

void parse_if(char **textp, FunctionBuilder *builder);

void parse_return(char **textp, FunctionBuilder *builder);

void parse_statement(char **textp, FunctionBuilder *builder);

void parse_block(char **textp, FunctionBuilder *builder);

UserFunction *parse_function_expr(char **textp);

UserFunction *parse_module(char **textp);

#endif
