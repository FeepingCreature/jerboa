#include "parser.h"

#include <stdio.h>
#include <stdbool.h>
#include <stdarg.h>

static bool starts_with(char **textp, char *cmp) {
  char *text = *textp;
  while (*cmp) {
    if (!text[0]) return false;
    if (text[0] != cmp[0]) return false;
    text++;
    cmp++;
  }
  *textp = text;
  return true;
}

void eat_whitespace(char **textp) {
  while ((*textp)[0] == ' ') (*textp)++;
}

bool eat_string(char **textp, char *keyword) {
  char *text = *textp;
  eat_whitespace(&text);
  if (starts_with(&text, keyword)) {
    *textp = text;
    return true;
  }
  return false;
}

char *parse_identifier(char **textp) {
  char *text = *textp;
  eat_whitespace(&text);
  char *start = text;
  if (text[0] && ((text[0] >= 'a' && text[0] <= 'z') || (text[0] >= 'A' && text[0] <= 'Z') || text[0] == '_')) text++;
  else return NULL;
  while (text[0] && ((text[0] >= 'a' && text[0] <= 'z') || (text[0] >= 'A' && text[0] <= 'Z') || (text[0] >= '0' && text[0] <= '9') || text[0] == '_')) text++;
  
  *textp = text;
  int len = text - start;
  char *res = malloc(len + 1);
  memcpy(res, start, len);
  res[len] = 0;
  return res;
}

bool parse_int(char **textp, int *outp) {
  char *text = *textp;
  eat_whitespace(&text);
  char *start = text;
  while (text[0] && text[0] >= '0' && text[0] <= '9') text++;
  if (text == start) return false;
  
  *textp = text;
  int len = text - start;
  char *res = malloc(len + 1);
  memcpy(res, start, len);
  res[len] = 0;
  *outp = atoi(res);
  free(res);
  return true;
}

bool eat_keyword(char **textp, char *keyword) {
  char *text = *textp;
  char *cmp = parse_identifier(&text);
  if (!cmp || strcmp(cmp, keyword) != 0) return false;
  *textp = text;
  return true;
}

void parser_error(char *location, char *format, ...) {
  fprintf(stderr, "at %.*s:\n", 20, location);
  fprintf(stderr, "parser error: ");
  va_list ap;
  va_start(ap, format);
  vfprintf(stderr, format, ap);
  va_end(ap);
  fprintf(stderr, "\n");
  exit(1);
}

void parser_error(char *location, char *format, ...) __attribute__ ((noreturn));

/*
 var foo = function(a) { var b = a; }
 - alloc scope
 - insert "foo" as null
 - close scope
 - generate closure bound to scope
 - assign closure to "foo"
*/

int parse_expr(char **textp, FunctionBuilder *builder, int level);

int parse_expr_base(char **textp, FunctionBuilder *builder) {
  char *text = *textp;
  char *ident_name = parse_identifier(&text);
  if (ident_name) {
    *textp = text;
    return addinstr_access(builder, builder->scope, ident_name);
  }
  
  int value;
  if (parse_int(&text, &value)) {
    *textp = text;
    
    int ctxslot = addinstr_get_context(builder);
    return addinstr_alloc_int_object(builder, ctxslot, value);
  }
  
  if (eat_string(&text, "(")) {
    int res = parse_expr(&text, builder, 0);
    if (!eat_string(&text, ")")) parser_error(text, "expected closing paren");
    
    *textp = text;
    return res;
  }
  
  parser_error(text, "expected identifier, number or paren expression");
}

bool parse_cont_call(char **textp, FunctionBuilder *builder, int *expr) {
  char *text = *textp;
  if (!(eat_string(&text, "("))) return false;
  
  *textp = text;
  int *args_ptr = NULL; int args_len = 0;
  
  while (!eat_string(textp, ")")) {
    if (args_len && !eat_string(textp, ",")) parser_error(*textp, "comma expected");
    int slot = parse_expr(textp, builder, 0);
    
    args_ptr = realloc(args_ptr, sizeof(int) * ++args_len);
    args_ptr[args_len - 1] = slot;
  }
  
  *expr = addinstr_call(builder, *expr, args_ptr, args_len);
  return true;
}

int parse_expr_base_tail(char **textp, FunctionBuilder *builder) {
  int expr = parse_expr_base(textp, builder);
  
  while (true) {
    if (parse_cont_call(textp, builder, &expr)) continue;
    break;
  }
  return expr;
}

/*
 * 0: ==
 * 1: + -
 * 2: * /
 */
int parse_expr(char **textp, FunctionBuilder *builder, int level) {
  char *text = *textp;
  
  int expr = parse_expr_base_tail(&text, builder);
  
  if (level > 2) { *textp = text; return expr; }
  
  while (true) {
    if (eat_string(&text, "*")) {
      int arg2 = parse_expr(&text, builder, 3);
      int ctxslot = addinstr_get_context(builder);
      int mulfn = addinstr_access(builder, ctxslot, "*");
      expr = addinstr_call2(builder, mulfn, expr, arg2);
      continue;
    }
    if (eat_string(&text, "/")) {
      int arg2 = parse_expr(&text, builder, 3);
      int ctxslot = addinstr_get_context(builder);
      int divfn = addinstr_access(builder, ctxslot, "/");
      expr = addinstr_call2(builder, divfn, expr, arg2);
      continue;
    }
    break;
  }
  
  if (level > 1) { *textp = text; return expr; }
  
  while (true) {
    if (eat_string(&text, "+")) {
      int arg2 = parse_expr(&text, builder, 2);
      int ctxslot = addinstr_get_context(builder);
      int plusfn = addinstr_access(builder, ctxslot, "+");
      expr = addinstr_call2(builder, plusfn, expr, arg2);
      continue;
    }
    if (eat_string(&text, "-")) {
      int arg2 = parse_expr(&text, builder, 2);
      int ctxslot = addinstr_get_context(builder);
      int minusfn = addinstr_access(builder, ctxslot, "-");
      expr = addinstr_call2(builder, minusfn, expr, arg2);
      continue;
    }
    break;
  }
  
  if (level > 0) { *textp = text; return expr; }
  
  while (true) {
    if (eat_string(&text, "==")) {
      int arg2 = parse_expr(&text, builder, 2);
      int ctxslot = addinstr_get_context(builder);
      int equalfn = addinstr_access(builder, ctxslot, "=");
      expr = addinstr_call2(builder, equalfn, expr, arg2);
      continue;
    }
    break;
  }
  
  *textp = text;
  return expr;
}

void parse_block(char **textp, FunctionBuilder *builder);

void parse_if(char **textp, FunctionBuilder *builder) {
  char *text = *textp;
  if (!eat_string(&text, "(")) parser_error(text, "if expected opening paren");
  int testslot = parse_expr(&text, builder, 0);
  if (!eat_string(&text, ")")) parser_error(text, "if expected closing paren");
  // blocks_ptr[0].instrs_ptr[6] = (Instr*) alloc(TestBranchInstr, {{INSTR_TESTBR}, 7, 1, 2});
  int *true_blk, *false_blk, *end_blk;
  addinstr_test_branch(builder, testslot, &true_blk, &false_blk);
  
  *true_blk = new_block(builder);
  parse_block(&text, builder);
  addinstr_branch(builder, &end_blk);
  
  *false_blk = new_block(builder);
  if (eat_string(&text, "else")) {
    parse_block(&text, builder);
    addinstr_branch(builder, &end_blk);
    *end_blk = new_block(builder);
  } else {
    *end_blk = *false_blk;
  }
  *textp = text;
}

void parse_return(char **textp, FunctionBuilder *builder) {
  int value = parse_expr(textp, builder, 0);
  if (!eat_string(textp, ";")) parser_error(*textp, "semicolon expected");
  addinstr_return(builder, value);
}

void parse_statement(char **textp, FunctionBuilder *builder) {
  char *text = *textp;
  if (eat_keyword(&text, "if")) {
    *textp = text;
    parse_if(textp, builder);
    return;
  }
  if (eat_keyword(&text, "return")) {
    *textp = text;
    parse_return(textp, builder);
    return;
  }
  parser_error(text, "unknown statement");
}

// note: blocks don't open new scopes, variable declarations do
// however, blocks reset the "active scope" at the end
// (this is important, because it allows us to close the active
//  scope after the variable declaration, allowing optimization later)

void parse_block(char **textp, FunctionBuilder *builder) {
  char *text = *textp;
  
  int scope_backup = builder->scope;
  
  if (eat_string(&text, "{")) {
    while (!eat_string(&text, "}")) {
      parse_statement(&text, builder);
    }
  } else {
    parse_statement(&text, builder);
  }
  
  *textp = text;
  builder->scope = scope_backup;
}

UserFunction *parse_function(char **textp) {
  char *text = *textp;
  if (!eat_keyword(&text, "function")) return NULL;
  char *fun_name = parse_identifier(&text);
  /*
  ┌────────┐  ┌─────┐
  │function│═▷│  (  │
  └────────┘  └─────┘
               ║   ║
               ▽   ║
      ┌───┐  ┌───┐ ║
      │ , │◁═│arg│ ║
      │   │═▷│   │ ║
      └───┘  └───┘ ║
               ║   ║
               ▽   ▽
              ┌─────┐
              │  )  │
              └─────┘
  */
  if (!eat_string(&text, "(")) parser_error(text, "opening paren for parameter list expected");
  char **arg_list_ptr = NULL, arg_list_len = 0;
  while (!eat_string(&text, ")")) {
    if (arg_list_len && !eat_string(&text, ",")) parser_error(text, "comma expected");
    char *arg = parse_identifier(&text);
    if (!arg) parser_error(text, "identifier expected");
    arg_list_ptr = realloc(arg_list_ptr, sizeof(char*) * ++arg_list_len);
    arg_list_ptr[arg_list_len - 1] = arg;
  }
  
  *textp = text;
  
  FunctionBuilder *builder = calloc(sizeof(FunctionBuilder), 1);
  builder->arglist_ptr = arg_list_ptr;
  builder->arglist_len = arg_list_len;
  builder->slot_base = arg_list_len;
  builder->name = fun_name;
  
  // generate lexical scope, initialize with parameters
  new_block(builder);
  int ctxslot = addinstr_get_context(builder);
  builder->scope = addinstr_alloc_object(builder, ctxslot);
  for (int i = 0; i < arg_list_len; ++i) {
    addinstr_assign(builder, builder->scope, arg_list_ptr[i], i);
  }
  addinstr_close_object(builder, builder->scope);
  
  parse_block(textp, builder);
  
  return build_function(builder);
}
