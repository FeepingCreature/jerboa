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

static void eat_whitespace(char **textp) {
  while ((*textp)[0] == ' ') (*textp)++;
}

void eat_filler(char **textp) {
  eat_whitespace(textp);
}

bool eat_string(char **textp, char *keyword) {
  char *text = *textp;
  eat_filler(&text);
  if (starts_with(&text, keyword)) {
    *textp = text;
    return true;
  }
  return false;
}

char *parse_identifier_all(char **textp) {
  char *text = *textp;
  eat_filler(&text);
  char *start = text;
  if (text[0] && ((text[0] >= 'a' && text[0] <= 'z') || (text[0] >= 'A' && text[0] <= 'Z') || text[0] == '_')) text++;
  else return NULL;
  while (text[0] && ((text[0] >= 'a' && text[0] <= 'z') || (text[0] >= 'A' && text[0] <= 'Z') || (text[0] >= '0' && text[0] <= '9') || text[0] == '_')) text++;
  
  int len = text - start;
  char *res = malloc(len + 1);
  memcpy(res, start, len);
  res[len] = 0;
  
  *textp = text;
  return res;
}

char *parse_identifier(char **textp) {
  char *text = *textp;
  char *res = parse_identifier_all(&text);
  if (res == NULL) return res;
  
  if (strncmp(res, "function", 8) == 0) {
    // reservdd identifier
    free(res);
    return NULL;
  }
  
  *textp = text;
  return res;
}

bool parse_int(char **textp, int *outp) {
  char *text = *textp;
  eat_filler(&text);
  char *start = text;
  if (text[0] && text[0] == '-') text++;
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

bool parse_float(char **textp, float *outp) {
  char *text = *textp;
  eat_filler(&text);
  char *start = text;
  if (text[0] && text[0] == '-') text++;
  while (text[0] && text[0] >= '0' && text[0] <= '9') text++;
  // has to be at least a . in there, or else it's an int
  if (!text[0] || text[0] != '.') return false;
  text++;
  while (text[0] && text[0] >= '0' && text[0] <= '9') text++;
  
  *textp = text;
  int len = text - start;
  char *res = malloc(len + 1);
  memcpy(res, start, len);
  res[len] = 0;
  *outp = atof(res);
  free(res);
  return true;
}

bool parse_string(char **textp, char **outp) {
  char *text = *textp;
  eat_filler(&text);
  char *start = text;
  if (text[0] != '"') return false;
  text++;
  while (text[0] != '"') text++; // TODO escape
  text++;
  
  *textp = text;
  int len = text - start;
  char *res = malloc(len - 2 + 1);
  memcpy(res, start + 1, len - 2);
  res[len - 2] = 0;
  *outp = res;
  return true;
}

bool eat_keyword(char **textp, char *keyword) {
  char *text = *textp;
  char *cmp = parse_identifier_all(&text);
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

int ref_access(FunctionBuilder *builder, RefValue rv) {
  if (!builder) return 0; // happens when speculatively parsing
  if (rv.key) {
    int key_slot = addinstr_alloc_string_object(builder, builder->scope, rv.key);
    return addinstr_access(builder, rv.base, key_slot);
  }
  return rv.base;
}

void ref_assign(FunctionBuilder *builder, RefValue rv, int value) {
  assert(rv.key);
  int key_slot = addinstr_alloc_string_object(builder, builder->scope, rv.key);
  addinstr_assign(builder, rv.base, key_slot, value);
}

void ref_assign_existing(FunctionBuilder *builder, RefValue rv, int value) {
  assert(rv.key);
  int key_slot = addinstr_alloc_string_object(builder, builder->scope, rv.key);
  addinstr_assign_existing(builder, rv.base, key_slot, value);
}

/*
 var foo = function(a) { var b = a; }
 - alloc scope
 - insert "foo" as null
 - close scope
 - generate closure bound to scope
 - assign closure to "foo"
*/

RefValue parse_expr(char **textp, FunctionBuilder *builder, int level);

static RefValue get_scope(FunctionBuilder *builder, char *name) {
  if (!builder) return (RefValue) {0, name, true};
  return (RefValue) {builder->scope, name, true};
}

static bool parse_object_literal(char **textp, FunctionBuilder *builder, RefValue *rv_p) {
  char *text = *textp;
  eat_filler(&text);
  
  if (!eat_string(&text, "{")) return false;
  
  int obj_slot = 0;
  if (builder) obj_slot = addinstr_alloc_object(builder, builder->slot_base++ /*null slot*/);
  
  while (!eat_string(&text, "}")) {
    char *key_name = parse_identifier(&text);
    if (!eat_string(&text, ":")) parser_error(text, "object literal expects 'name: value'");
    
    RefValue value = parse_expr(&text, builder, 0);
    if (builder) {
      int key_slot = addinstr_alloc_string_object(builder, builder->scope, key_name);
      int value_slot = ref_access(builder, value);
      addinstr_assign(builder, obj_slot, key_slot, value_slot);
    }
    
    if (eat_string(&text, ",")) continue;
    if (eat_string(&text, "}")) break;
    parser_error(text, "expected commad or closing bracket");
  }
  *textp = text;
  *rv_p = (RefValue) {obj_slot, NULL, false};
  return true;
}

RefValue parse_expr_stem(char **textp, FunctionBuilder *builder) {
  char *text = *textp;
  char *ident_name = parse_identifier(&text);
  if (ident_name) {
    *textp = text;
    return get_scope(builder, ident_name);
  }
  
  float f_value;
  if (parse_float(&text, &f_value)) {
    *textp = text;
    if (!builder) return (RefValue) {0, NULL, false};
    int slot = addinstr_alloc_float_object(builder, builder->scope, f_value);
    return (RefValue) {slot, NULL, false};
  }
  
  int i_value;
  if (parse_int(&text, &i_value)) {
    *textp = text;
    if (!builder) return (RefValue) {0, NULL, false};
    int slot = addinstr_alloc_int_object(builder, builder->scope, i_value);
    return (RefValue) {slot, NULL, false};
  }
  
  char *t_value;
  if (parse_string(&text, &t_value)) {
    *textp = text;
    if (!builder) return (RefValue) {0, NULL, false};
    int slot = addinstr_alloc_string_object(builder, builder->scope, t_value);
    return (RefValue) {slot, NULL, false};
  }
  
  RefValue rv;
  if (parse_object_literal(&text, builder, &rv)) {
    *textp = text;
    return rv;
  }
  
  if (eat_string(&text, "(")) {
    RefValue res = parse_expr(&text, builder, 0);
    if (!eat_string(&text, ")")) parser_error(text, "'()' expression expected closing paren");
    
    *textp = text;
    return res;
  }
  
  if (eat_keyword(&text, "function")) {
    UserFunction *fn = parse_function_expr(&text);
    if (!builder) return (RefValue) {0, NULL, false};
    int slot = addinstr_alloc_closure_object(builder, builder->scope, fn);
    *textp = text;
    return (RefValue) {slot, NULL, false};
  }
  
  parser_error(text, "expected expression");
}

bool parse_cont_call(char **textp, FunctionBuilder *builder, RefValue *expr) {
  char *text = *textp;
  if (!eat_string(&text, "(")) return false;
  
  *textp = text;
  int *args_ptr = NULL; int args_len = 0;
  
  while (!eat_string(textp, ")")) {
    if (args_len && !eat_string(textp, ",")) parser_error(*textp, "comma expected");
    RefValue arg = parse_expr(textp, builder, 0);
    int slot = 0;
    if (builder) slot = ref_access(builder, arg);
    
    args_ptr = realloc(args_ptr, sizeof(int) * ++args_len);
    args_ptr[args_len - 1] = slot;
  }
  
  if (!builder) return true;
  
  *expr = (RefValue) {
    addinstr_call(builder, ref_access(builder, *expr), args_ptr, args_len),
    NULL,
    false
  };
  return true;
}

bool parse_prop_access(char **textp, FunctionBuilder *builder, RefValue *expr) {
  char *text = *textp;
  if (!eat_string(&text, ".")) return false;
  
  char *keyname = parse_identifier(&text);
  *textp = text;
  
  *expr = (RefValue) {ref_access(builder, *expr), keyname, false};
  return true;
}

RefValue parse_expr_base(char **textp, FunctionBuilder *builder) {
  RefValue expr = parse_expr_stem(textp, builder);
  
  while (true) {
    if (parse_cont_call(textp, builder, &expr)) continue;
    if (parse_prop_access(textp, builder, &expr)) continue;
    break;
  }
  return expr;
}

/*
 * 0: ==
 * 1: + -
 * 2: * /
 */
RefValue parse_expr(char **textp, FunctionBuilder *builder, int level) {
  char *text = *textp;
  
  RefValue expr = parse_expr_base(&text, builder);
  
  if (level > 2) { *textp = text; return expr; }
  
  while (true) {
    if (eat_string(&text, "*")) {
      int arg2 = ref_access(builder, parse_expr(&text, builder, 3));
      if (!builder) continue;
      int mulfn = addinstr_access(builder, builder->scope, addinstr_alloc_string_object(builder, builder->scope, "*"));
      expr = (RefValue) { addinstr_call2(builder, mulfn, ref_access(builder, expr), arg2), NULL, false };
      continue;
    }
    if (eat_string(&text, "/")) {
      int arg2 = ref_access(builder, parse_expr(&text, builder, 3));
      if (!builder) continue;
      int divfn = addinstr_access(builder, builder->scope, addinstr_alloc_string_object(builder, builder->scope, "/"));
      expr = (RefValue) { addinstr_call2(builder, divfn, ref_access(builder, expr), arg2), NULL, false };
      continue;
    }
    break;
  }
  
  if (level > 1) { *textp = text; return expr; }
  
  while (true) {
    if (eat_string(&text, "+")) {
      int arg2 = ref_access(builder, parse_expr(&text, builder, 2));
      if (!builder) continue;
      int plusfn = addinstr_access(builder, builder->scope, addinstr_alloc_string_object(builder, builder->scope, "+"));
      expr = (RefValue) { addinstr_call2(builder, plusfn, ref_access(builder, expr), arg2), NULL, false };
      continue;
    }
    if (eat_string(&text, "-")) {
      int arg2 = ref_access(builder, parse_expr(&text, builder, 2));
      if (!builder) continue;
      int minusfn = addinstr_access(builder, builder->scope, addinstr_alloc_string_object(builder, builder->scope, "-"));
      expr = (RefValue) { addinstr_call2(builder, minusfn, ref_access(builder, expr), arg2), NULL, false };
      continue;
    }
    break;
  }
  
  if (level > 0) { *textp = text; return expr; }
  
  if (eat_string(&text, "==")) {
    int arg2 = ref_access(builder, parse_expr(&text, builder, 1));
    if (builder) {
      int equalfn = addinstr_access(builder, builder->scope, addinstr_alloc_string_object(builder, builder->scope, "="));
      expr = (RefValue) { addinstr_call2(builder, equalfn, ref_access(builder, expr), arg2), NULL, false };
    }
  } else if (eat_string(&text, "<")) {
    int arg2 = ref_access(builder, parse_expr(&text, builder, 1));
    if (builder) {
      int smallerfn = addinstr_access(builder, builder->scope, addinstr_alloc_string_object(builder, builder->scope, "<"));
      expr = (RefValue) { addinstr_call2(builder, smallerfn, ref_access(builder, expr), arg2), NULL, false };
    }
  }
  
  *textp = text;
  return expr;
}

void parse_block(char **textp, FunctionBuilder *builder);

void parse_if(char **textp, FunctionBuilder *builder) {
  char *text = *textp;
  if (!eat_string(&text, "(")) parser_error(text, "if expected opening paren");
  int testslot = ref_access(builder, parse_expr(&text, builder, 0));
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
  int value = ref_access(builder, parse_expr(textp, builder, 0));
  if (!eat_string(textp, ";")) parser_error(*textp, "semicolon expected");
  addinstr_return(builder, value);
}

void parse_vardecl(char **textp, FunctionBuilder *builder) {
  // allocate the new scope immediately, so that the variable
  // is in scope for the value expression.
  // (this is important for recursion, ie. var foo = function() { foo(); }; )
  builder->scope = addinstr_alloc_object(builder, builder->scope);
  
  char *varname = parse_identifier(textp);
  int value, varname_slot = addinstr_alloc_string_object(builder, builder->scope, varname);
  if (!eat_string(textp, "=")) {
    value = builder->slot_base++; // null slot
  } else {
    value = ref_access(builder, parse_expr(textp, builder, 0));
  }
  
  addinstr_assign(builder, builder->scope, varname_slot, value);
  addinstr_close_object(builder, builder->scope);
  
  // var a, b;
  if (eat_string(textp, ",")) {
    parse_vardecl(textp, builder);
    return;
  }
  
  if (!eat_string(textp, ";")) parser_error(*textp, "';' expected to close 'var' decl");
}

void parse_fundecl(char **textp, FunctionBuilder *builder) {
  // alloc scope for fun var
  builder->scope = addinstr_alloc_object(builder, builder->scope);
  
  UserFunction *fn = parse_function_expr(textp);
  int name_slot = addinstr_alloc_string_object(builder, builder->scope, fn->name);
  int slot = addinstr_alloc_closure_object(builder, builder->scope, fn);
  addinstr_assign(builder, builder->scope, name_slot, slot);
  addinstr_close_object(builder, builder->scope);
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
  if (eat_keyword(&text, "var")) {
    *textp = text;
    parse_vardecl(textp, builder);
    return;
  }
  if (eat_keyword(&text, "function")) {
    *textp = text;
    parse_fundecl(textp, builder);
    return;
  }
  {
    char *text2 = text;
    parse_expr_base(&text2, NULL); // speculative
    if (eat_string(&text2, "=")) {
      RefValue target = parse_expr_base(&text, builder);
      
      if (!eat_string(&text, "=")) assert(false); // Internal inconsistency
      
      int value = ref_access(builder, parse_expr(&text, builder, 0));
      if (target.is_variable) {
        ref_assign_existing(builder, target, value);
      } else {
        ref_assign(builder, target, value);
      }
      
      if (!eat_string(&text, ";")) parser_error(text, "';' expected to close assignment");
      *textp = text;
      return;
    }
  }
  {
    // expr as statement
    parse_expr_base(&text, builder);
    if (!eat_string(&text, ";")) parser_error(text, "';' expected to terminate expression");
    *textp = text;
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

UserFunction *parse_function_expr(char **textp) {
  char *text = *textp;
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
    int argname_slot = addinstr_alloc_string_object(builder, builder->scope, arg_list_ptr[i]);
    addinstr_assign(builder, builder->scope, argname_slot, i);
  }
  addinstr_close_object(builder, builder->scope);
  
  parse_block(textp, builder);
  
  return build_function(builder);
}

UserFunction *parse_module(char **textp) {
  FunctionBuilder *builder = calloc(sizeof(FunctionBuilder), 1);
  builder->slot_base = 0;
  builder->name = NULL;
  
  new_block(builder);
  builder->scope = addinstr_get_context(builder);
  
  while (true) {
    eat_filler(textp);
    if ((*textp)[0] == 0) break;
    parse_statement(textp, builder);
  }
  addinstr_return(builder, builder->scope);
  return build_function(builder);
}
