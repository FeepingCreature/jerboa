#ifndef PARSER_H
#define PARSER_H

#include "vm/builder.h"

typedef enum {
  REFMODE_NONE = -1, // not a reference
  REFMODE_VARIABLE, // find the key; if you find it, overwrite it; otherwise error
  REFMODE_OBJECT, // find the key; if you find it, shadow it; otherwise error
  REFMODE_INDEX // just write the key without checking for anything
} RefMode;

// if 'key' is -1, the value is just 'base'
typedef struct {
  int base;
  int key;
  RefMode mode;
} RefValue;

void eat_filler(char **textp);

bool eat_string(char **textp, char *keyword);

bool eat_keyword(char **textp, char *keyword);

char *parse_identifier_all(char **textp);

char *parse_identifier(char **textp);

bool parse_int(char **textp, int *outp);

bool parse_float(char **textp, float *outp);

void parser_error(char *location, char *format, ...) __attribute__ ((noreturn));

int ref_access(FunctionBuilder *builder, RefValue rv);

void ref_assign_existing(FunctionBuilder *builder, RefValue rv, int value);

RefValue parse_expr(char **textp, FunctionBuilder *builder, int level);

RefValue parse_expr_base(char **textp, FunctionBuilder *builder);

bool parse_cont_call(char **textp, FunctionBuilder *builder, RefValue *expr);

RefValue parse_expr_base_tail(char **textp, FunctionBuilder *builder);

RefValue parse_expr(char **textp, FunctionBuilder *builder, int level);

void parse_block(char **textp, FunctionBuilder *builder);

void parse_if(char **textp, FunctionBuilder *builder);

void parse_return(char **textp, FunctionBuilder *builder);

void parse_vardecl(char **textp, FunctionBuilder *builder);

void parse_fundecl(char **textp, FunctionBuilder *builder);

void parse_statement(char **textp, FunctionBuilder *builder);

void parse_block(char **textp, FunctionBuilder *builder);

UserFunction *parse_function_expr(char **textp);

UserFunction *parse_module(char **textp);

#endif
