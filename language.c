#include "language.h"

#include "parser.h"
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

static UserFunction *parse_function_expr(char **textp);

static int ref_access(FunctionBuilder *builder, RefValue rv) {
  if (!builder) return 0; // happens when speculatively parsing
  if (rv.key != -1) {
    return addinstr_access(builder, rv.base, rv.key);
  }
  return rv.base;
}

static void ref_assign(FunctionBuilder *builder, RefValue rv, int value) {
  assert(rv.key != -1);
  addinstr_assign(builder, rv.base, rv.key, value, ASSIGN_PLAIN);
}

static void ref_assign_existing(FunctionBuilder *builder, RefValue rv, int value) {
  assert(rv.key != -1);
  addinstr_assign(builder, rv.base, rv.key, value, ASSIGN_EXISTING);
}

static void ref_assign_shadowing(FunctionBuilder *builder, RefValue rv, int value) {
  assert(rv.key != -1);
  addinstr_assign(builder, rv.base, rv.key, value, ASSIGN_SHADOWING);
}

/*
 var foo = function(a) { var b = a; }
 - alloc scope
 - insert "foo" as null
 - close scope
 - generate closure bound to scope
 - assign closure to "foo"
*/

static RefValue parse_expr(char **textp, FunctionBuilder *builder, int level);

static RefValue get_scope(FunctionBuilder *builder, char *name) {
  if (!builder) return (RefValue) {0, -1, REFMODE_VARIABLE};
  int name_slot = addinstr_alloc_string_object(builder, builder->scope, name);
  return (RefValue) {builder->scope, name_slot, REFMODE_VARIABLE};
}

static void parse_object_literal_body(char **textp, FunctionBuilder *builder, int obj_slot) {
  while (!eat_string(textp, "}")) {
    char *key_name = parse_identifier(textp);
    if (!eat_string(textp, ":")) parser_error(*textp, "object literal expects 'name: value'");
    
    RefValue value = parse_expr(textp, builder, 0);
    if (builder) {
      int key_slot = addinstr_alloc_string_object(builder, builder->scope, key_name);
      int value_slot = ref_access(builder, value);
      addinstr_assign(builder, obj_slot, key_slot, value_slot, ASSIGN_PLAIN);
    }
    
    if (eat_string(textp, ",")) continue;
    if (eat_string(textp, "}")) break;
    parser_error(*textp, "expected commad or closing bracket");
  }
}

static bool parse_object_literal(char **textp, FunctionBuilder *builder, RefValue *rv_p) {
  char *text = *textp;
  eat_filler(&text);
  
  if (!eat_string(&text, "{")) return false;
  
  int obj_slot = 0;
  if (builder) obj_slot = addinstr_alloc_object(builder, builder->slot_base++ /*null slot*/);
  *textp = text;
  *rv_p = (RefValue) {obj_slot, -1, REFMODE_NONE};
  
  parse_object_literal_body(textp, builder, obj_slot);
  
  return true;
}

static void parse_array_literal_body(char **textp, FunctionBuilder *builder, int obj_slot) {
  RefValue *values_ptr = NULL; int values_len = 0;
  while (!eat_string(textp, "]")) {
    RefValue value = parse_expr(textp, builder, 0);
    if (builder) {
      values_ptr = realloc(values_ptr, sizeof(RefValue) * ++values_len);
      values_ptr[values_len - 1] = value;
    }
    if (eat_string(textp, ",")) continue;
    if (eat_string(textp, "]")) break;
    parser_error(*textp, "expected commad or closing square bracket");
  }
  if (builder) {
    int keyslot1 = addinstr_alloc_string_object(builder, builder->scope, "resize");
    int keyslot2 = addinstr_alloc_string_object(builder, builder->scope, "[]=");
    int resizefn = ref_access(builder, (RefValue) {obj_slot, keyslot1, REFMODE_OBJECT});
    int assignfn = ref_access(builder, (RefValue) {obj_slot, keyslot2, REFMODE_OBJECT});
    int newsize_slot = addinstr_alloc_int_object(builder, builder->scope, values_len);
    obj_slot = addinstr_call1(builder, resizefn, obj_slot, newsize_slot);
    for (int i = 0; i < values_len; ++i) {
      int index_slot = addinstr_alloc_int_object(builder, builder->scope, i);
      addinstr_call2(builder, assignfn, obj_slot, index_slot, ref_access(builder, values_ptr[i]));
    }
  }
  free(values_ptr);
}

static bool parse_array_literal(char **textp, FunctionBuilder *builder, RefValue *rv_p) {
  char *text = *textp;
  eat_filler(&text);
  
  if (!eat_string(&text, "[")) return false;
  
  int obj_slot = 0;
  if (builder) obj_slot = addinstr_alloc_array_object(builder, builder->scope);
  *textp = text;
  *rv_p = (RefValue) {obj_slot, -1, REFMODE_NONE};
  
  parse_array_literal_body(textp, builder, obj_slot);
  
  return true;
}

static RefValue parse_expr_stem(char **textp, FunctionBuilder *builder) {
  char *text = *textp;
  char *ident_name = parse_identifier(&text);
  if (ident_name) {
    *textp = text;
    return get_scope(builder, ident_name);
  }
  
  float f_value;
  if (parse_float(&text, &f_value)) {
    *textp = text;
    if (!builder) return (RefValue) {0, -1, REFMODE_NONE};
    int slot = addinstr_alloc_float_object(builder, builder->scope, f_value);
    return (RefValue) {slot, -1, false};
  }
  
  int i_value;
  if (parse_int(&text, &i_value)) {
    *textp = text;
    if (!builder) return (RefValue) {0, -1, REFMODE_NONE};
    int slot = addinstr_alloc_int_object(builder, builder->scope, i_value);
    return (RefValue) {slot, -1, REFMODE_NONE};
  }
  
  char *t_value;
  if (parse_string(&text, &t_value)) {
    *textp = text;
    if (!builder) return (RefValue) {0, -1, REFMODE_NONE};
    int slot = addinstr_alloc_string_object(builder, builder->scope, t_value);
    return (RefValue) {slot, -1, REFMODE_NONE};
  }
  
  RefValue rv;
  if (parse_object_literal(&text, builder, &rv)) {
    *textp = text;
    return rv;
  }
  if (parse_array_literal(&text, builder, &rv)) {
    *textp = text;
    return rv;
  }
  
  if (eat_string(&text, "(")) {
    RefValue res = parse_expr(&text, builder, 0);
    if (!eat_string(&text, ")")) parser_error(text, "'()' expression expected closing paren");
    
    *textp = text;
    return res;
  }
  
  bool is_method = false;
  if (eat_keyword(&text, "function") || (eat_keyword(&text, "method") && (is_method = true))) {
    UserFunction *fn = parse_function_expr(&text);
    if (!builder) return (RefValue) {0, -1, REFMODE_NONE};
    fn->is_method = is_method;
    int slot = addinstr_alloc_closure_object(builder, builder->scope, fn);
    *textp = text;
    return (RefValue) {slot, -1, REFMODE_NONE};
  }
  
  if (eat_keyword(&text, "new")) {
    RefValue parent_var = parse_expr(&text, builder, 0);
    int parent_slot = ref_access(builder, parent_var);
    int obj_slot = 0;
    if (builder) obj_slot = addinstr_alloc_object(builder, parent_slot);
    
    *textp = text;
    if (eat_string(textp, "{")) {
      parse_object_literal_body(textp, builder, obj_slot);
    }
    return (RefValue) {obj_slot, -1, REFMODE_NONE};
  }
  
  parser_error(text, "expected expression");
}

static bool parse_cont_call(char **textp, FunctionBuilder *builder, RefValue *expr) {
  char *text = *textp;
  if (!eat_string(&text, "(")) return false;
  
  *textp = text;
  int *args_ptr = NULL; int args_len = 0;
  
  while (!eat_string(textp, ")")) {
    if (args_len && !eat_string(textp, ",")) parser_error(*textp, "comma expected");
    RefValue arg = parse_expr(textp, builder, 0);
    int slot = ref_access(builder, arg);
    
    args_ptr = realloc(args_ptr, sizeof(int) * ++args_len);
    args_ptr[args_len - 1] = slot;
  }
  
  if (!builder) return true;
  
  int this_slot;
  if (expr->key) this_slot = expr->base;
  else this_slot = builder->slot_base++; /* null slot */
  
  *expr = (RefValue) {
    addinstr_call(builder, ref_access(builder, *expr), this_slot, args_ptr, args_len),
    -1,
    REFMODE_NONE
  };
  return true;
}

static bool parse_array_access(char **textp, FunctionBuilder *builder, RefValue *expr) {
  char *text = *textp;
  if (!eat_string(&text, "[")) return false;
  
  *textp = text;
  RefValue key = parse_expr(textp, builder, 0);
  
  if (!eat_string(textp, "]")) parser_error(*textp, "closing ']' expected");
  
  int key_slot = ref_access(builder, key);
  
  *expr = (RefValue) {ref_access(builder, *expr), key_slot, REFMODE_INDEX};
  return true;
}

static bool parse_prop_access(char **textp, FunctionBuilder *builder, RefValue *expr) {
  char *text = *textp;
  if (!eat_string(&text, ".")) return false;
  
  char *keyname = parse_identifier(&text);
  *textp = text;
  
  int key_slot = 0;
  if (builder) key_slot = addinstr_alloc_string_object(builder, builder->scope, keyname);

  *expr = (RefValue) {ref_access(builder, *expr), key_slot, REFMODE_OBJECT};
  return true;
}

static RefValue parse_expr_base(char **textp, FunctionBuilder *builder) {
  RefValue expr = parse_expr_stem(textp, builder);
  
  while (true) {
    if (parse_cont_call(textp, builder, &expr)) continue;
    if (parse_prop_access(textp, builder, &expr)) continue;
    if (parse_array_access(textp, builder, &expr)) continue;
    break;
  }
  return expr;
}

/*
 * 0: == != < > <= >=
 * 1: + -
 * 2: * /
 */
static RefValue parse_expr(char **textp, FunctionBuilder *builder, int level) {
  char *text = *textp;
  
  RefValue expr = parse_expr_base(&text, builder);
  
  if (level > 2) { *textp = text; return expr; }
  
  while (true) {
    if (eat_string(&text, "*")) {
      int rhs_value = ref_access(builder, parse_expr(&text, builder, 3));
      if (!builder) continue;
      int lhs_value = ref_access(builder, expr);
      int mulfn = addinstr_access(builder, lhs_value, addinstr_alloc_string_object(builder, builder->scope, "*"));
      expr = (RefValue) { addinstr_call1(builder, mulfn, lhs_value, rhs_value), -1, REFMODE_NONE };
      continue;
    }
    if (eat_string(&text, "/")) {
      int rhs_value = ref_access(builder, parse_expr(&text, builder, 3));
      if (!builder) continue;
      int lhs_value = ref_access(builder, expr);
      int divfn = addinstr_access(builder, lhs_value, addinstr_alloc_string_object(builder, builder->scope, "/"));
      expr = (RefValue) { addinstr_call1(builder, divfn, lhs_value, rhs_value), -1, REFMODE_NONE };
      continue;
    }
    break;
  }
  
  if (level > 1) { *textp = text; return expr; }
  
  while (true) {
    if (eat_string(&text, "+")) {
      int rhs_value = ref_access(builder, parse_expr(&text, builder, 2));
      if (!builder) continue;
      int lhs_value = ref_access(builder, expr);
      int plusfn = addinstr_access(builder, lhs_value, addinstr_alloc_string_object(builder, builder->scope, "+"));
      expr = (RefValue) { addinstr_call1(builder, plusfn, lhs_value, rhs_value), -1, REFMODE_NONE };
      continue;
    }
    if (eat_string(&text, "-")) {
      int rhs_value = ref_access(builder, parse_expr(&text, builder, 2));
      if (!builder) continue;
      int lhs_value = ref_access(builder, expr);
      int minusfn = addinstr_access(builder, lhs_value, addinstr_alloc_string_object(builder, builder->scope, "-"));
      expr = (RefValue) { addinstr_call1(builder, minusfn, lhs_value, rhs_value), -1, REFMODE_NONE };
      continue;
    }
    break;
  }
  
  if (level > 0) { *textp = text; return expr; }
  
  bool negate_expr = false;
  
  if (eat_string(&text, "==")) {
    int rhs_value = ref_access(builder, parse_expr(&text, builder, 1));
    if (builder) {
      int lhs_value = ref_access(builder, expr);
      int equalfn = addinstr_access(builder, lhs_value, addinstr_alloc_string_object(builder, builder->scope, "=="));
      expr = (RefValue) { addinstr_call1(builder, equalfn, lhs_value, rhs_value), -1, REFMODE_NONE };
    }
  } else if (eat_string(&text, "!=")) {
    int rhs_value = ref_access(builder, parse_expr(&text, builder, 1));
    if (builder) {
      int lhs_value = ref_access(builder, expr);
      int equalfn = addinstr_access(builder, lhs_value, addinstr_alloc_string_object(builder, builder->scope, "=="));
      expr = (RefValue) { addinstr_call1(builder, equalfn, lhs_value, rhs_value), -1, REFMODE_NONE };
      negate_expr = true;
    }
  } else {
    if (eat_string(&text, "!")) {
      negate_expr = true;
    }
    if (eat_string(&text, "<=")) {
      int rhs_value = ref_access(builder, parse_expr(&text, builder, 1));
      if (builder) {
        int lhs_value = ref_access(builder, expr);
        int lefn = addinstr_access(builder, lhs_value, addinstr_alloc_string_object(builder, builder->scope, "<="));
        expr = (RefValue) { addinstr_call1(builder, lefn, lhs_value, rhs_value), -1, REFMODE_NONE };
      }
    } else if (eat_string(&text, ">=")) {
      int rhs_value = ref_access(builder, parse_expr(&text, builder, 1));
      if (builder) {
        int lhs_value = ref_access(builder, expr);
        int gefn = addinstr_access(builder, lhs_value, addinstr_alloc_string_object(builder, builder->scope, ">="));
        expr = (RefValue) { addinstr_call1(builder, gefn, lhs_value, rhs_value), -1, REFMODE_NONE };
      }
    } else if (eat_string(&text, "<")) {
      int rhs_value = ref_access(builder, parse_expr(&text, builder, 1));
      if (builder) {
        int lhs_value = ref_access(builder, expr);
        int ltfn = addinstr_access(builder, lhs_value, addinstr_alloc_string_object(builder, builder->scope, "<"));
        expr = (RefValue) { addinstr_call1(builder, ltfn, lhs_value, rhs_value), -1, REFMODE_NONE };
      }
    } else if (eat_string(&text, ">")) {
      int rhs_value = ref_access(builder, parse_expr(&text, builder, 1));
      if (builder) {
        int lhs_value = ref_access(builder, expr);
        int gtfn = addinstr_access(builder, lhs_value, addinstr_alloc_string_object(builder, builder->scope, ">"));
        expr = (RefValue) { addinstr_call1(builder, gtfn, lhs_value, rhs_value), -1, REFMODE_NONE };
      }
    } else if (negate_expr) {
      parser_error(text, "expected comparison operator");
    }
  }
  
  if (negate_expr && builder) {
    int lhs_value = ref_access(builder, expr);
    int notfn = addinstr_access(builder, lhs_value, addinstr_alloc_string_object(builder, builder->scope, "!"));
    expr = (RefValue) { addinstr_call0(builder, notfn, lhs_value), -1, REFMODE_NONE };
  }
  
  *textp = text;
  return expr;
}

static void parse_block(char **textp, FunctionBuilder *builder);

static void parse_if(char **textp, FunctionBuilder *builder) {
  char *text = *textp;
  if (!eat_string(&text, "(")) parser_error(text, "if expected opening paren");
  int testslot = ref_access(builder, parse_expr(&text, builder, 0));
  if (!eat_string(&text, ")")) parser_error(text, "if expected closing paren");
  int *true_blk, *false_blk, *end_blk;
  addinstr_test_branch(builder, testslot, &true_blk, &false_blk);
  
  *true_blk = new_block(builder);
  parse_block(&text, builder);
  addinstr_branch(builder, &end_blk);
  
  *false_blk = new_block(builder);
  if (eat_keyword(&text, "else")) {
    parse_block(&text, builder);
    addinstr_branch(builder, &end_blk);
    *end_blk = new_block(builder);
  } else {
    *end_blk = *false_blk;
  }
  *textp = text;
}

static void parse_while(char **textp, FunctionBuilder *builder) {
  char *text = *textp;
  if (!eat_string(&text, "(")) parser_error(text, "'while' expected opening paren");
  
  int *test_blk;
  addinstr_branch(builder, &test_blk);
  *test_blk = new_block(builder);
  
  int *loop_blk, *end_blk;
  int testslot = ref_access(builder, parse_expr(&text, builder, 0));
  if (!eat_string(&text, ")")) parser_error(text, "'while' expected closing paren");
  addinstr_test_branch(builder, testslot, &loop_blk, &end_blk);
  
  *loop_blk = new_block(builder);
  parse_block(&text, builder);
  int *test_blk2;
  addinstr_branch(builder, &test_blk2);
  *test_blk2 = *test_blk;
  
  *end_blk = new_block(builder);
  *textp = text;
}

static void parse_return(char **textp, FunctionBuilder *builder) {
  int value = ref_access(builder, parse_expr(textp, builder, 0));
  if (!eat_string(textp, ";")) parser_error(*textp, "semicolon expected");
  addinstr_return(builder, value);
  new_block(builder);
}

static void parse_vardecl(char **textp, FunctionBuilder *builder) {
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
  
  addinstr_assign(builder, builder->scope, varname_slot, value, ASSIGN_PLAIN);
  addinstr_close_object(builder, builder->scope);
  
  // var a, b;
  if (eat_string(textp, ",")) {
    parse_vardecl(textp, builder);
    return;
  }
  
  if (!eat_string(textp, ";")) parser_error(*textp, "';' expected to close 'var' decl");
}

static void parse_fundecl(char **textp, FunctionBuilder *builder) {
  // alloc scope for fun var
  builder->scope = addinstr_alloc_object(builder, builder->scope);
  
  UserFunction *fn = parse_function_expr(textp);
  int name_slot = addinstr_alloc_string_object(builder, builder->scope, fn->name);
  int slot = addinstr_alloc_closure_object(builder, builder->scope, fn);
  addinstr_assign(builder, builder->scope, name_slot, slot, ASSIGN_PLAIN);
  addinstr_close_object(builder, builder->scope);
}

static void parse_statement(char **textp, FunctionBuilder *builder) {
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
  if (eat_keyword(&text, "while")) {
    *textp = text;
    parse_while(textp, builder);
    return;
  }
  {
    char *text2 = text;
    parse_expr_base(&text2, NULL); // speculative
    if (eat_string(&text2, "=")) {
      RefValue target = parse_expr_base(&text, builder);
      
      if (!eat_string(&text, "=")) assert(false); // Internal inconsistency
      
      int value = ref_access(builder, parse_expr(&text, builder, 0));
      
      switch (target.mode) {
        case REFMODE_VARIABLE: ref_assign_existing(builder, target, value); break;
        case REFMODE_OBJECT: ref_assign_shadowing(builder, target, value); break;
        case REFMODE_INDEX: ref_assign(builder, target, value); break;
        default: assert(false);
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

static void parse_block(char **textp, FunctionBuilder *builder) {
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

static UserFunction *parse_function_expr(char **textp) {
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
  builder->block_terminated = true;
  
  // generate lexical scope, initialize with parameters
  new_block(builder);
  int ctxslot = addinstr_get_context(builder);
  builder->scope = addinstr_alloc_object(builder, ctxslot);
  for (int i = 0; i < arg_list_len; ++i) {
    int argname_slot = addinstr_alloc_string_object(builder, builder->scope, arg_list_ptr[i]);
    addinstr_assign(builder, builder->scope, argname_slot, i, ASSIGN_PLAIN);
  }
  addinstr_close_object(builder, builder->scope);
  
  parse_block(textp, builder);
  terminate(builder);
  
  return build_function(builder);
}

UserFunction *parse_module(char **textp) {
  FunctionBuilder *builder = calloc(sizeof(FunctionBuilder), 1);
  builder->slot_base = 0;
  builder->name = NULL;
  builder->block_terminated = true;
  
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
