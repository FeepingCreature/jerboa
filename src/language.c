#include "language.h"

#include <stdio.h>

#include "parser.h"
#include "vm/builder.h"
#include "vm/optimize.h"

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
  FileRange *range;
} RefValue;

static ParseResult parse_function_expr(char **textp, UserFunction **uf);

static RefValue ref_simple(int slot) {
  return (RefValue) { slot, -1, REFMODE_NONE, NULL };
}

static int ref_access(FunctionBuilder *builder, RefValue rv) {
  if (!builder) return 0; // happens when speculatively parsing
  if (rv.key != -1) {
    bool use_range = builder->current_range != rv.range;
    if (use_range) use_range_start(builder, rv.range);
    int res = addinstr_access(builder, rv.base, rv.key);
    if (use_range) use_range_end(builder, rv.range);
    return res;
  }
  return rv.base;
}

static void ref_assign(FunctionBuilder *builder, RefValue rv, int value) {
  if (!builder) return;
  assert(rv.key != -1);
  addinstr_assign(builder, rv.base, rv.key, value, ASSIGN_PLAIN);
}

static void ref_assign_existing(FunctionBuilder *builder, RefValue rv, int value) {
  if (!builder) return;
  assert(rv.key != -1);
  addinstr_assign(builder, rv.base, rv.key, value, ASSIGN_EXISTING);
}

static void ref_assign_shadowing(FunctionBuilder *builder, RefValue rv, int value) {
  if (!builder) return;
  assert(rv.key != -1);
  addinstr_assign(builder, rv.base, rv.key, value, ASSIGN_SHADOWING);
}

// note: lexical scopes don't create new vm scopes, variable declarations do
// however, lexical scopes reset the "active scope" at the end
// (this is important, because it allows us to close the active
//  scope immediately after the variable declaration, allowing optimization later)

// IMPORTANT: because this is pure, you do NOT need to call end_lex_scope before returning errors!
static int begin_lex_scope(FunctionBuilder *builder) {
  return builder->scope;
}

static void end_lex_scope(FunctionBuilder *builder, int backup) {
  builder->scope = backup;
}

/*
 var foo = function(a) { var b = a; }
 - alloc scope
 - insert "foo" as null
 - close scope
 - generate closure bound to scope
 - assign closure to "foo"
*/

static ParseResult parse_expr(char **textp, FunctionBuilder *builder, int level, RefValue *rv);

static RefValue get_scope(FunctionBuilder *builder, char *name) {
  if (!builder) return (RefValue) {0, -1, REFMODE_VARIABLE, NULL};
  assert(builder->current_range);
  int name_slot = addinstr_alloc_string_object(builder, name);
  return (RefValue) {builder->scope, name_slot, REFMODE_VARIABLE, builder->current_range};
}

static ParseResult parse_object_literal_body(char **textp, FunctionBuilder *builder, int obj_slot) {
  char *text = *textp;
  while (!eat_string(&text, "}")) {
    FileRange *add_entry_range = alloc_and_record_start(text);
    char *key_name = parse_identifier(&text);
    if (!key_name) {
      ParseResult res = parse_string(&text, &key_name);
      if (res != PARSE_OK) {
        log_parser_error(text, "identifier or identifier literal expected");
        free(add_entry_range);
        return PARSE_ERROR;
      }
    }
    record_end(text, add_entry_range);
    
    if (!eat_string(&text, "=")) {
      log_parser_error(text, "object literal expects 'name = value;'");
      return PARSE_ERROR;
    }
    
    RefValue value;
    ParseResult res = parse_expr(&text, builder, 0, &value);
    if (res == PARSE_ERROR) return res;
    assert(res == PARSE_OK);
    
    if (!eat_string(&text, ";")) {
      log_parser_error(text, "object literal entry requires terminating semicolon");
      return PARSE_ERROR;
    }
    
    if (builder) {
      int value_slot = ref_access(builder, value);
      
      use_range_start(builder, add_entry_range);
      int key_slot = addinstr_alloc_string_object(builder, key_name);
      addinstr_assign(builder, obj_slot, key_slot, value_slot, ASSIGN_PLAIN);
      use_range_end(builder, add_entry_range);
    }
  }
  *textp = text;
  return PARSE_OK;
}

static ParseResult parse_object_literal(char **textp, FunctionBuilder *builder, RefValue *rv_p) {
  char *text = *textp;
  FileRange *range = alloc_and_record_start(text);
  if (!eat_string(&text, "{")) {
    free(range);
    return PARSE_NONE;
  }
  
  int obj_slot = 0;
  use_range_start(builder, range);
  if (builder) obj_slot = addinstr_alloc_object(builder, 0);
  use_range_end(builder, range);
  *textp = text;
  *rv_p = ref_simple(obj_slot);
  
  ParseResult res = parse_object_literal_body(textp, builder, obj_slot);
  record_end(text, range);
  return res;
}

static ParseResult parse_array_literal_body(char **textp, FunctionBuilder *builder, int obj_slot, FileRange *range) {
  char *text = *textp;
  RefValue *values_ptr = NULL; int values_len = 0;
  while (!eat_string(&text, "]")) {
    RefValue value;
    ParseResult res = parse_expr(&text, builder, 0, &value);
    if (res == PARSE_ERROR) return res;
    assert(res == PARSE_OK);
    
    if (builder) {
      values_ptr = realloc(values_ptr, sizeof(RefValue) * ++values_len);
      values_ptr[values_len - 1] = value;
    }
    if (eat_string(&text, ",")) continue;
    if (eat_string(&text, "]")) break;
    log_parser_error(text, "expected commad or closing square bracket");
    return PARSE_ERROR;
  }
  *textp = text;
  if (builder) {
    use_range_start(builder, range);
    int keyslot1 = addinstr_alloc_string_object(builder, "resize");
    int keyslot2 = addinstr_alloc_string_object(builder, "[]=");
    int resizefn = ref_access(builder, (RefValue) {obj_slot, keyslot1, REFMODE_OBJECT, range});
    int assignfn = ref_access(builder, (RefValue) {obj_slot, keyslot2, REFMODE_OBJECT, range});
    int newsize_slot = addinstr_alloc_int_object(builder, values_len);
    obj_slot = addinstr_call1(builder, resizefn, obj_slot, newsize_slot);
    for (int i = 0; i < values_len; ++i) {
      int index_slot = addinstr_alloc_int_object(builder, i);
      use_range_end(builder, range);
      int value_slot = ref_access(builder, values_ptr[i]);
      use_range_start(builder, range);
      addinstr_call2(builder, assignfn, obj_slot, index_slot, value_slot);
    }
    use_range_end(builder, range);
  }
  free(values_ptr);
  return PARSE_OK;
}

static ParseResult parse_array_literal(char **textp, FunctionBuilder *builder, RefValue *rv_p) {
  char *text = *textp;
  FileRange *lit_range = alloc_and_record_start(text);
  if (!eat_string(&text, "[")) {
    free(lit_range);
    return PARSE_NONE;
  }
  
  int obj_slot = 0;
  use_range_start(builder, lit_range);
  if (builder) obj_slot = addinstr_alloc_array_object(builder);
  *textp = text;
  *rv_p = ref_simple(obj_slot);
  use_range_end(builder, lit_range);
  
  ParseResult res = parse_array_literal_body(textp, builder, obj_slot, lit_range);
  record_end(text, lit_range);
  return res;
}

static ParseResult parse_expr_stem(char **textp, FunctionBuilder *builder, RefValue *rv) {
  char *text = *textp;
  
  FileRange *range = alloc_and_record_start(text);
  
  char *ident_name = parse_identifier(&text);
  if (ident_name) {
    *textp = text;
    record_end(text, range);
    use_range_start(builder, range);
    *rv = get_scope(builder, ident_name);
    use_range_end(builder, range);
    return PARSE_OK;
  }
  
  float f_value;
  if (parse_float(&text, &f_value)) {
    *textp = text;
    record_end(text, range);
    int slot = 0;
    if (builder) {
      use_range_start(builder, range);
      slot = addinstr_alloc_float_object(builder, f_value);
      use_range_end(builder, range);
    }
    *rv = ref_simple(slot);
    return PARSE_OK;
  }
  
  int i_value;
  if (parse_int(&text, &i_value)) {
    *textp = text;
    record_end(text, range);
    int slot = 0;
    if (builder) {
      use_range_start(builder, range);
      slot = addinstr_alloc_int_object(builder, i_value);
      use_range_end(builder, range);
    }
    *rv = ref_simple(slot);
    return PARSE_OK;
  }
  
  ParseResult res;
  
  char *t_value;
  if ((res = parse_string(&text, &t_value)) != PARSE_NONE) {
    int slot = 0;
    if (res == PARSE_OK) {
      *textp = text;
      record_end(text, range);
      if (builder) {
        use_range_start(builder, range);
        slot = addinstr_alloc_string_object(builder, t_value);
        use_range_end(builder, range);
      }
    }
    *rv = ref_simple(slot);
    return res;
  }
  
  if ((res = parse_object_literal(&text, builder, rv)) != PARSE_NONE) {
    if (res == PARSE_OK) *textp = text;
    free(range);
    return res;
  }
  
  if ((res = parse_array_literal(&text, builder, rv)) != PARSE_NONE) {
    if (res == PARSE_OK) *textp = text;
    free(range);
    return res;
  }
  
  if (eat_string(&text, "(")) {
    free(range);
    
    res = parse_expr(&text, builder, 0, rv);
    if (res == PARSE_ERROR) return res;
    assert(res == PARSE_OK);
    
    if (!eat_string(&text, ")")) {
      log_parser_error(text, "'()' expression expected closing paren");
      return PARSE_ERROR;
    }
    
    *textp = text;
    return PARSE_OK;
  }
  
  bool is_method = false;
  if (eat_keyword(&text, "function") || (eat_keyword(&text, "method") && (is_method = true))) {
    record_end(text, range);
    
    UserFunction *fn;
    ParseResult res = parse_function_expr(&text, &fn);
    if (res == PARSE_ERROR) return res;
    assert(res == PARSE_OK);
    *textp = text;
    
    int slot = 0;
    if (builder) {
      fn->is_method = is_method;
      use_range_start(builder, range); // count the closure allocation under 'function'
      slot = addinstr_alloc_closure_object(builder, fn);
      use_range_end(builder, range);
    }
    *rv = ref_simple(slot);
    return PARSE_OK;
  }
  
  if (eat_keyword(&text, "new")) {
    record_end(text, range);
    
    RefValue parent_var;
    ParseResult res = parse_expr(&text, builder, 0, &parent_var);
    if (res == PARSE_ERROR) return res;
    assert(res == PARSE_OK);
    
    int parent_slot = ref_access(builder, parent_var);
    int obj_slot = 0;
    use_range_start(builder, range);
    if (builder) obj_slot = addinstr_alloc_object(builder, parent_slot);
    use_range_end(builder, range);
    
    *textp = text;
    if (eat_string(textp, "{")) {
      ParseResult res = parse_object_literal_body(textp, builder, obj_slot);
      if (res == PARSE_ERROR) return res;
      assert(res == PARSE_OK);
    }
    *rv = ref_simple(obj_slot);
    return PARSE_OK;
  }
  free(range);
  
  log_parser_error(text, "expected expression");
  return PARSE_ERROR;
}

static ParseResult parse_cont_call(char **textp, FunctionBuilder *builder, RefValue *expr, FileRange *expr_range_dyn) {
  char *text = *textp;
  FileRange *call_range = alloc_and_record_start(text);
  FileRange *expr_range = malloc(sizeof(FileRange));
  *expr_range = *expr_range_dyn;
  
  if (!eat_string(&text, "(")) {
    free(call_range);
    return PARSE_NONE;
  }
  
  int *args_ptr = NULL; int args_len = 0;
  
  while (!eat_string(&text, ")")) {
    if (args_len && !eat_string(&text, ",")) {
      log_parser_error(text, "comma expected");
      return PARSE_ERROR;
    }
    RefValue arg;
    ParseResult res = parse_expr(&text, builder, 0, &arg);
    if (res == PARSE_ERROR) return res;
    assert(res == PARSE_OK);
    
    args_ptr = realloc(args_ptr, sizeof(int) * ++args_len);
    args_ptr[args_len - 1] = ref_access(builder, arg);
  }
  
  record_end(text, call_range);
  
  *textp = text;
  if (builder) {
    int this_slot;
    // scopes are not a valid form of "this"
    if (expr->key) this_slot = expr->base;
    else this_slot = 0;
    
    int expr_slot = ref_access(builder, *expr);
    
    // use_range_start(builder, call_range);
    use_range_start(builder, expr_range);
    *expr = ref_simple(addinstr_call(builder, expr_slot, this_slot, args_ptr, args_len));
    // use_range_end(builder, call_range);
    use_range_end(builder, expr_range);
  }
  return PARSE_OK;
}

static ParseResult parse_array_access(char **textp, FunctionBuilder *builder, RefValue *expr) {
  char *text = *textp;
  FileRange *access_range = alloc_and_record_start(text);
  if (!eat_string(&text, "[")) {
    free(access_range);
    return PARSE_NONE;
  }
  
  RefValue key;
  ParseResult res = parse_expr(&text, builder, 0, &key);
  if (res == PARSE_ERROR) return res;
  assert(res == PARSE_OK);
  
  if (!eat_string(&text, "]")) {
    log_parser_error(*textp, "closing ']' expected");
    free(access_range);
    return PARSE_ERROR;
  }
  record_end(text, access_range);
  
  *textp = text;
  
  int key_slot = ref_access(builder, key);
  int expr_slot = ref_access(builder, *expr);
  
  *expr = (RefValue) {expr_slot, key_slot, REFMODE_INDEX, access_range};
  
  return PARSE_OK;
}

static ParseResult parse_prop_access(char **textp, FunctionBuilder *builder, RefValue *expr) {
  char *text = *textp;
  
  FileRange *prop_range = alloc_and_record_start(text);
  if (!eat_string(&text, ".")) {
    free(prop_range);
    return PARSE_NONE;
  }
  
  char *keyname = parse_identifier(&text);
  record_end(text, prop_range);
  *textp = text;
  
  int expr_slot = 0;
  int key_slot = 0;
  if (builder) expr_slot = ref_access(builder, *expr);
  use_range_start(builder, prop_range);
  if (builder) key_slot = addinstr_alloc_string_object(builder, keyname);
  *expr = (RefValue) {expr_slot, key_slot, REFMODE_OBJECT, prop_range};
  use_range_end(builder, prop_range);
  
  return PARSE_OK;
}

void build_op(FunctionBuilder *builder, char *op, RefValue *res_rv, RefValue lhs_expr, RefValue rhs_expr, FileRange *range) {
  if (builder) {
    int lhs_value = ref_access(builder, lhs_expr);
    int rhs_value = ref_access(builder, rhs_expr);
    use_range_start(builder, range);
    int fn = addinstr_access(builder, lhs_value, addinstr_alloc_string_object(builder, op));
    *res_rv = ref_simple(addinstr_call1(builder, fn, lhs_value, rhs_value));
    use_range_end(builder, range);
  }
}

static bool assign_value(FunctionBuilder *builder, RefValue rv, int value, FileRange *assign_range) {
  switch (rv.mode) {
    case REFMODE_NONE:
      use_range_end(builder, assign_range);
      free(assign_range);
      return false;
    case REFMODE_VARIABLE: ref_assign_existing(builder, rv, value); break;
    case REFMODE_OBJECT: ref_assign_shadowing(builder, rv, value); break;
    case REFMODE_INDEX: ref_assign(builder, rv, value); break;
    default: abort();
  }
  use_range_end(builder, assign_range);
  return true;
}

static ParseResult parse_postincdec(char **textp, FunctionBuilder *builder, RefValue *rv) {
  char *text = *textp;
  FileRange *op_range = alloc_and_record_start(text);
  char *op = NULL;
  if (eat_string(&text, "++")) {
    op = "+";
  } else if (eat_string(&text, "--")) {
    op = "-";
  } else return PARSE_NONE;
  
  record_end(text, op_range);
  
  int prev_slot = 0, one_slot = 0;
  if (builder) {
    prev_slot = ref_access(builder, *rv);
    use_range_start(builder, op_range);
    one_slot = addinstr_alloc_int_object(builder, 1);
    use_range_end(builder, op_range);
  }
  
  RefValue sum;
  build_op(builder, op, &sum, *rv, ref_simple(one_slot), op_range);
  use_range_start(builder, op_range);
  if (!assign_value(builder, *rv, ref_access(builder, sum), op_range)) {
    log_parser_error(*textp, "post-inc/dec cannot assign to non-reference expression!");
    return PARSE_ERROR;
  }
  *textp = text;
  *rv = ref_simple(prev_slot);
  return PARSE_OK;
}

static ParseResult parse_expr_base(char **textp, FunctionBuilder *builder, RefValue *rv) {
  char *text = *textp;
  
  FileRange *expr_range = alloc_and_record_start(text);
  ParseResult res = parse_expr_stem(&text, builder, rv);
  if (res == PARSE_ERROR) {
    free(expr_range);
    return PARSE_ERROR;
  }
  assert(res == PARSE_OK);
  
  while (true) {
    record_end(text, expr_range);
    if ((res = parse_cont_call(&text, builder, rv, expr_range)) == PARSE_OK) continue;
    if (res == PARSE_ERROR) return res;
    if ((res = parse_prop_access(&text, builder, rv)) == PARSE_OK) continue;
    if (res == PARSE_ERROR) return res;
    if ((res = parse_array_access(&text, builder, rv)) == PARSE_OK) continue;
    if (res == PARSE_ERROR) return res;
    if ((res = parse_postincdec(&text, builder, rv)) == PARSE_OK) continue;
    if (res == PARSE_ERROR) return res;
    break;
  }
  
  *textp = text;
  return PARSE_OK;
}

/*
 * 0: == != < > <= >=
 * 1: + -
 * 2: * /
 */
static ParseResult parse_expr(char **textp, FunctionBuilder *builder, int level, RefValue *rv) {
  char *text = *textp;
  
  bool negate = false;
  FileRange *neg_range = alloc_and_record_start(text);
  if (eat_string(&text, "-")) {
    record_end(text, neg_range);
    negate = true;
  } else free(neg_range);
  
  ParseResult res = parse_expr_base(&text, builder, rv);
  if (res == PARSE_ERROR) return PARSE_ERROR;
  assert(res == PARSE_OK);
  
  if (negate) {
    use_range_start(builder, neg_range);
    int zero_slot = 0;
    if (builder) zero_slot = addinstr_alloc_int_object(builder, 0);
    use_range_end(builder, neg_range);
    RefValue zref = ref_simple(zero_slot);
    build_op(builder, "-", rv, zref, *rv, neg_range);
  }
  
  if (level > 5) { *textp = text; return PARSE_OK; }
  
  RefValue rhs_expr;
  while (true) {
    FileRange *range = alloc_and_record_start(text);
    if (eat_string(&text, "&")) {
      record_end(text, range);
      res = parse_expr(&text, builder, 6, &rhs_expr);
      if (res == PARSE_ERROR) return PARSE_ERROR; assert(res == PARSE_OK);
      build_op(builder, "&", rv, *rv, rhs_expr, range);
      continue;
    }
    free(range);
    break;
  }
  
  if (level > 4) { *textp = text; return PARSE_OK; }
  
  while (true) {
    FileRange *range = alloc_and_record_start(text);
    if (eat_string(&text, "|")) {
      record_end(text, range);
      res = parse_expr(&text, builder, 5, &rhs_expr);
      if (res == PARSE_ERROR) return PARSE_ERROR; assert(res == PARSE_OK);
      build_op(builder, "|", rv, *rv, rhs_expr, range);
      continue;
    }
    free(range);
    break;
  }
  
  if (level > 3) { *textp = text; return PARSE_OK; }
  
  while (true) {
    FileRange *range = alloc_and_record_start(text);
    if (eat_string(&text, "*")) {
      record_end(text, range);
      res = parse_expr(&text, builder, 4, &rhs_expr);
      if (res == PARSE_ERROR) return PARSE_ERROR; assert(res == PARSE_OK);
      build_op(builder, "*", rv, *rv, rhs_expr, range);
      continue;
    }
    if (eat_string(&text, "/")) {
      record_end(text, range);
      res = parse_expr(&text, builder, 4, &rhs_expr);
      if (res == PARSE_ERROR) return PARSE_ERROR; assert(res == PARSE_OK);
      build_op(builder, "/", rv, *rv, rhs_expr, range);
      continue;
    }
    free(range);
    break;
  }
  
  if (level > 2) { *textp = text; return PARSE_OK; }
  
  while (true) {
    FileRange *range = alloc_and_record_start(text);
    if (eat_string(&text, "+")) {
      record_end(text, range);
      res = parse_expr(&text, builder, 3, &rhs_expr);
      if (res == PARSE_ERROR) return PARSE_ERROR; assert(res == PARSE_OK);
      build_op(builder, "+", rv, *rv, rhs_expr, range);
      continue;
    }
    if (eat_string(&text, "-")) {
      record_end(text, range);
      res = parse_expr(&text, builder, 3, &rhs_expr);
      if (res == PARSE_ERROR) return PARSE_ERROR; assert(res == PARSE_OK);
      build_op(builder, "-", rv, *rv, rhs_expr, range);
      continue;
    }
    free(range);
    break;
  }
  
  if (level > 1) { *textp = text; return PARSE_OK; }
  
  while (true) {
    FileRange *range = alloc_and_record_start(text);
    if (eat_keyword(&text, "in")) {
      record_end(text, range);
      res = parse_expr(&text, builder, 2, &rhs_expr);
      if (res == PARSE_ERROR) return PARSE_ERROR; assert(res == PARSE_OK);
      int key_slot = ref_access(builder, *rv);
      int obj_slot = ref_access(builder, rhs_expr);
      int in_slot = 0;
      use_range_start(builder, range);
      if (builder) in_slot = addinstr_key_in_obj(builder, key_slot, obj_slot);
      use_range_end(builder, range);
      *rv = ref_simple(in_slot);
      continue;
    }
    free(range);
    break;
  }
  
  if (level > 0) { *textp = text; return PARSE_OK; }
  
  bool negate_expr = false;
  
  FileRange *range = alloc_and_record_start(text);
  if (eat_string(&text, "==")) {
    record_end(text, range);
    res = parse_expr(&text, builder, 1, &rhs_expr);
    if (res == PARSE_ERROR) return PARSE_ERROR; assert(res == PARSE_OK);
    build_op(builder, "==", rv, *rv, rhs_expr, range);
  } else if (eat_string(&text, "!=")) {
    record_end(text, range);
    res = parse_expr(&text, builder, 1, &rhs_expr);
    if (res == PARSE_ERROR) return PARSE_ERROR; assert(res == PARSE_OK);
    build_op(builder, "==", rv, *rv, rhs_expr, range);
    negate_expr = true;
  } else {
    if (eat_string(&text, "!")) {
      negate_expr = true;
    }
    if (eat_string(&text, "<=")) {
      record_end(text, range);
      res = parse_expr(&text, builder, 1, &rhs_expr);
      if (res == PARSE_ERROR) return PARSE_ERROR; assert(res == PARSE_OK);
      build_op(builder, "<=", rv, *rv, rhs_expr, range);
    } else if (eat_string(&text, ">=")) {
      record_end(text, range);
      res = parse_expr(&text, builder, 1, &rhs_expr);
      if (res == PARSE_ERROR) return PARSE_ERROR; assert(res == PARSE_OK);
      build_op(builder, ">=", rv, *rv, rhs_expr, range);
    } else if (eat_string(&text, "<")) {
      record_end(text, range);
      res = parse_expr(&text, builder, 1, &rhs_expr);
      if (res == PARSE_ERROR) return PARSE_ERROR; assert(res == PARSE_OK);
      build_op(builder, "<", rv, *rv, rhs_expr, range);
    } else if (eat_string(&text, ">")) {
      record_end(text, range);
      res = parse_expr(&text, builder, 1, &rhs_expr);
      if (res == PARSE_ERROR) return PARSE_ERROR; assert(res == PARSE_OK);
      build_op(builder, ">", rv, *rv, rhs_expr, range);
    } else if (negate_expr) {
      free(range);
      log_parser_error(text, "expected comparison operator");
      return PARSE_ERROR;
    }
  }
  
  if (negate_expr && builder) {
    use_range_start(builder, range);
    int lhs_value = ref_access(builder, *rv);
    int notfn = addinstr_access(builder, lhs_value, addinstr_alloc_string_object(builder, "!"));
    *rv = ref_simple(addinstr_call0(builder, notfn, lhs_value));
    use_range_end(builder, range);
  }
  
  *textp = text;
  return PARSE_OK;
}

static ParseResult parse_block(char **textp, FunctionBuilder *builder, bool force_brackets);

static ParseResult parse_if(char **textp, FunctionBuilder *builder, FileRange *keywd_range) {
  char *text = *textp;
  if (!eat_string(&text, "(")) {
    log_parser_error(text, "if expected opening paren");
    return PARSE_ERROR;
  }
  RefValue test_expr;
  ParseResult res = parse_expr(&text, builder, 0, &test_expr);
  if (res == PARSE_ERROR) return PARSE_ERROR;
  assert(res == PARSE_OK);
  int testslot = ref_access(builder, test_expr);
  if (!eat_string(&text, ")")) {
    log_parser_error(text, "if expected closing paren");
    return PARSE_ERROR;
  }
  IntVarRef true_blk, false_blk, end_blk;
  use_range_start(builder, keywd_range);
  addinstr_test_branch(builder, testslot, &true_blk, &false_blk);
  use_range_end(builder, keywd_range);
  
  set_int_var(builder, true_blk, new_block(builder));
  
  res = parse_block(&text, builder, false);
  if (res == PARSE_ERROR) return PARSE_ERROR;
  assert(res == PARSE_OK);
  
  use_range_start(builder, keywd_range);
  addinstr_branch(builder, &end_blk);
  use_range_end(builder, keywd_range);
  
  int false_blk_idx = new_block(builder);
  set_int_var(builder, false_blk, false_blk_idx);
  if (eat_keyword(&text, "else")) {
    res = parse_block(&text, builder, false);
    if (res == PARSE_ERROR) return PARSE_ERROR;
    assert(res == PARSE_OK);
    
    IntVarRef end_blk2;
    use_range_start(builder, keywd_range);
    addinstr_branch(builder, &end_blk2);
    use_range_end(builder, keywd_range);
    int blk_idx = new_block(builder);
    set_int_var(builder, end_blk2, blk_idx);
    set_int_var(builder, end_blk, blk_idx);
  } else {
    set_int_var(builder, end_blk, false_blk_idx);
  }
  *textp = text;
  return PARSE_OK;
}

static ParseResult parse_while(char **textp, FunctionBuilder *builder, FileRange *range) {
  char *text = *textp;
  if (!eat_string(&text, "(")) {
    log_parser_error(text, "'while' expected opening paren");
    return PARSE_ERROR;
  }
  
  use_range_start(builder, range);
  IntVarRef test_blk;
  addinstr_branch(builder, &test_blk);
  int test_blk_idx = new_block(builder);
  set_int_var(builder, test_blk, test_blk_idx);
  use_range_end(builder, range);
  
  IntVarRef loop_blk, end_blk;
  
  RefValue test_expr;
  ParseResult res = parse_expr(&text, builder, 0, &test_expr);
  if (res == PARSE_ERROR) return PARSE_ERROR;
  assert(res == PARSE_OK);
  
  int testslot = ref_access(builder, test_expr);
  if (!eat_string(&text, ")")) {
    log_parser_error(text, "'while' expected closing paren");
    return PARSE_ERROR;
  }
  use_range_start(builder, range);
  addinstr_test_branch(builder, testslot, &loop_blk, &end_blk);
  use_range_end(builder, range);
  
  set_int_var(builder, loop_blk, new_block(builder));
  res = parse_block(&text, builder, false);
  if (res == PARSE_ERROR) return PARSE_ERROR;
  assert(res == PARSE_OK);
  
  use_range_start(builder, range);
  IntVarRef test_blk2;
  addinstr_branch(builder, &test_blk2);
  use_range_end(builder, range);
  
  set_int_var(builder, test_blk2, test_blk_idx);
  set_int_var(builder, end_blk, new_block(builder));
  
  *textp = text;
  return PARSE_OK;
}

static ParseResult parse_vardecl(char **textp, FunctionBuilder *builder, FileRange *var_range, bool isconst) {
  char *text = *textp;
  
  FileRange *alloc_var_name = alloc_and_record_start(text);
  char *varname = parse_identifier(&text);
  if (!varname) {
    log_parser_error(text, "invalid identifier for variable");
    return PARSE_ERROR;
  }
  record_end(text, alloc_var_name);
  
  FileRange *define_constraint = alloc_and_record_start(text);
  int constraint_slot = -1;
  if (eat_string(&text, ":")) {
    char *constraint = parse_identifier(&text);
    if (!constraint) {
      log_parser_error(text, "invalid type constraint");
      return PARSE_ERROR;
    }
    record_end(text, define_constraint);
    use_range_start(builder, define_constraint);
    int constraint_name_slot = addinstr_alloc_string_object(builder, constraint);
    constraint_slot = addinstr_access(builder, builder->scope, constraint_name_slot);
    use_range_end(builder, define_constraint);
  }
  
  // allocate the new scope upfront, so that the variable
  // is in scope for the value expression.
  // (this is important for recursion, ie. var foo = function() { foo(); }; )
  use_range_start(builder, var_range);
  builder->scope = addinstr_alloc_object(builder, builder->scope);
  int var_scope = builder->scope; // in case we later decide that expressions can open new scopes
  use_range_end(builder, var_range);
  
  use_range_start(builder, alloc_var_name);
  int value, varname_slot = addinstr_alloc_string_object(builder, varname);
  addinstr_assign(builder, var_scope, varname_slot, 0, ASSIGN_PLAIN);
  addinstr_close_object(builder, var_scope);
  use_range_end(builder, alloc_var_name);
  
  FileRange *assign_value = alloc_and_record_start(text);
  
  if (!eat_string(&text, "=")) {
    free(assign_value);
    assign_value = alloc_var_name;
    value = 0;
  } else {
    record_end(text, assign_value);
    RefValue rv;
    
    FileRange *calc_init_expr = alloc_and_record_start(text);
    ParseResult res = parse_expr(&text, builder, 0, &rv);
    record_end(text, calc_init_expr);
    
    if (res == PARSE_ERROR) return res;
    assert(res == PARSE_OK);
    
    value = ref_access(builder, rv);
  }
  
  use_range_start(builder, assign_value);
  addinstr_assign(builder, var_scope, varname_slot, value, ASSIGN_EXISTING);
  use_range_end(builder, assign_value);
  
  if (constraint_slot != -1) {
    use_range_start(builder, define_constraint);
    addinstr_set_constraint(builder, var_scope, varname_slot, constraint_slot);
    use_range_end(builder, define_constraint);
  }
  
  use_range_start(builder, assign_value);
  if (isconst) addinstr_freeze_object(builder, var_scope);
  use_range_end(builder, assign_value);
  
  // var a, b;
  if (eat_string(&text, ",")) {
    *textp = text;
    return parse_vardecl(textp, builder, var_range, isconst);
  }
  
  *textp = text;
  return PARSE_OK;
}

static ParseResult parse_assign(char **textp, FunctionBuilder *builder) {
  char *text2 = *textp;
  RefValue rv;
  ParseResult res = parse_expr_base(&text2, NULL, &rv); // speculative
  if (res != PARSE_OK) return res;
  
  char *op = NULL, *assign_text = NULL;
  if (eat_string(&text2, "+=")) { op = "+"; assign_text = "+="; }
  else if (eat_string(&text2, "-=")) { op = "-"; assign_text = "-="; }
  else if (eat_string(&text2, "*=")) { op = "*"; assign_text = "*="; }
  else if (eat_string(&text2, "/=")) { op = "/"; assign_text = "/="; }
  else if (eat_string(&text2, "=")) { assign_text = "="; }
  else return PARSE_NONE;
  
  // discard text2, redo with non-null builder
  char *text = *textp;
  res = parse_expr_base(&text, builder, &rv);
  assert(res == PARSE_OK);
  FileRange *assign_range = alloc_and_record_start(text);
  if (!eat_string(&text, assign_text)) abort(); // Internal inconsistency
  record_end(text, assign_range);
  
  RefValue value_expr;
  res = parse_expr(&text, builder, 0, &value_expr);
  if (res == PARSE_ERROR) {
    free(assign_range);
    return res;
  }
  assert(res == PARSE_OK);
  
  if (op) {
    build_op(builder, op, &value_expr, rv, value_expr, assign_range);
  }
  
  int value = ref_access(builder, value_expr);
  
  use_range_start(builder, assign_range);
  if (!assign_value(builder, rv, value, assign_range)) {
    log_parser_error(text, "cannot assign to non-reference expression!");
    return PARSE_ERROR;
  }
  
  *textp = text;
  return PARSE_OK;
}

static ParseResult parse_semicolon_statement(char **textp, FunctionBuilder *builder);

static ParseResult parse_for(char **textp, FunctionBuilder *builder, FileRange *range) {
  char *text = *textp;
  if (!eat_string(&text, "(")) {
    log_parser_error(text, "'for' expected opening paren");
    return PARSE_ERROR;
  }
  
  // variable is out of scope after the loop
  int scope_backup = begin_lex_scope(builder);
  
  FileRange *decl_range = alloc_and_record_start(text);
  if (eat_keyword(&text, "var")) {
    ParseResult res = parse_vardecl(&text, builder, decl_range, false);
    if (res == PARSE_ERROR) return res;
    assert(res == PARSE_OK);
  } else {
    ParseResult res = parse_assign(&text, builder);
    if (res == PARSE_ERROR) return res;
    assert(res == PARSE_OK);
  }
  
  if (!eat_string(&text, ";")) {
    log_parser_error(text, "'for' expected semicolon");
    return PARSE_ERROR;
  }
  
  use_range_start(builder, range);
  IntVarRef test_blk;
  addinstr_branch(builder, &test_blk);
  int test_blk_idx = new_block(builder);
  set_int_var(builder, test_blk, test_blk_idx);
  use_range_end(builder, range);
  
  IntVarRef loop_blk, end_blk;
  
  RefValue test_expr;
  ParseResult res = parse_expr(&text, builder, 0, &test_expr);
  if (res == PARSE_ERROR) return PARSE_ERROR;
  assert(res == PARSE_OK);
  
  if (!eat_string(&text, ";")) {
    log_parser_error(text, "'for' expected semicolon");
    return PARSE_ERROR;
  }
  
  char *text_step = text; // keep a text pointer to the step statement
  {
    // skip ahead to the loop statement
    res = parse_semicolon_statement(&text, NULL);
    if (res == PARSE_ERROR) return PARSE_ERROR;
    if (res == PARSE_NONE) {
      log_parser_error(text, "'for' expected assignment");
      return PARSE_ERROR;
    }
    assert(res == PARSE_OK);
    if (!eat_string(&text, ")")) {
      log_parser_error(text, "'for' expected closing paren");
      return PARSE_ERROR;
    }
  }
  
  use_range_start(builder, range);
  int testslot = ref_access(builder, test_expr);

  addinstr_test_branch(builder, testslot, &loop_blk, &end_blk);
  use_range_end(builder, range);
  
  // begin loop body
  set_int_var(builder, loop_blk, new_block(builder));
  res = parse_block(&text, builder, false);
  if (res == PARSE_ERROR) return PARSE_ERROR;
  assert(res == PARSE_OK);
  
  res = parse_semicolon_statement(&text_step, builder);
  assert(res == PARSE_OK); // what? this already worked above...
  
  use_range_start(builder, range);
  IntVarRef test_blk2;
  addinstr_branch(builder, &test_blk2);
  set_int_var(builder, test_blk2, test_blk_idx);
  
  use_range_end(builder, range);
  
  set_int_var(builder, end_blk, new_block(builder));
  
  end_lex_scope(builder, scope_backup);
  
  *textp = text;
  return PARSE_OK;
}

static ParseResult parse_return(char **textp, FunctionBuilder *builder, FileRange *keywd_range) {
  RefValue ret_value;
  ParseResult res = parse_expr(textp, builder, 0, &ret_value);
  if (res == PARSE_ERROR) return res;
  assert(res == PARSE_OK);
  
  int value = ref_access(builder, ret_value);
  
  use_range_start(builder, keywd_range);
  addinstr_return(builder, value);
  new_block(builder);
  use_range_end(builder, keywd_range);
  return PARSE_OK;
}

static ParseResult parse_fundecl(char **textp, FunctionBuilder *builder, FileRange *range) {
  // alloc scope for fun var
  use_range_start(builder, range);
  builder->scope = addinstr_alloc_object(builder, builder->scope);
  use_range_end(builder, range);
  
  UserFunction *fn;
  ParseResult res = parse_function_expr(textp, &fn);
  if (res == PARSE_ERROR) return res;
  assert(res == PARSE_OK);
  use_range_start(builder, range);
  int name_slot = addinstr_alloc_string_object(builder, fn->name);
  int slot = addinstr_alloc_closure_object(builder, fn);
  addinstr_assign(builder, builder->scope, name_slot, slot, ASSIGN_PLAIN);
  addinstr_close_object(builder, builder->scope);
  addinstr_freeze_object(builder, builder->scope);
  use_range_end(builder, range);
  return PARSE_OK;
}

static ParseResult parse_semicolon_statement(char **textp, FunctionBuilder *builder) {
  char *text = *textp;
  FileRange *keyword_range = alloc_and_record_start(text);
  ParseResult res;
  if (eat_keyword(&text, "return")) {
    record_end(text, keyword_range);
    res = parse_return(&text, builder, keyword_range);
  }
  else if (eat_keyword(&text, "var")) {
    record_end(text, keyword_range);
    res = parse_vardecl(&text, builder, keyword_range, false);
  }
  else if (eat_keyword(&text, "const")) {
    record_end(text, keyword_range);
    res = parse_vardecl(&text, builder, keyword_range, true);
  }
  else {
    res = parse_assign(&text, builder);
    if (res == PARSE_NONE) {
      // expr as statement
      RefValue rv;
      res = parse_expr_base(&text, builder, &rv);
      if (res == PARSE_NONE) {
        return PARSE_NONE;
      }
    }
  }
  
  if (res == PARSE_ERROR) return PARSE_ERROR;
  assert(res == PARSE_OK);
  *textp = text;
  return PARSE_OK;
}

static ParseResult parse_statement(char **textp, FunctionBuilder *builder) {
  char *text = *textp;
  FileRange *keyword_range = alloc_and_record_start(text);
  if (eat_keyword(&text, "if")) {
    record_end(text, keyword_range);
    *textp = text;
    return parse_if(textp, builder, keyword_range);
  }
  if (eat_keyword(&text, "function")) {
    record_end(text, keyword_range);
    *textp = text;
    return parse_fundecl(textp, builder, keyword_range);
  }
  if (eat_keyword(&text, "while")) {
    record_end(text, keyword_range);
    *textp = text;
    return parse_while(textp, builder, keyword_range);
  }
  if (eat_keyword(&text, "for")) {
    record_end(text, keyword_range);
    *textp = text;
    return parse_for(textp, builder, keyword_range);
  }
  ParseResult res = parse_block(&text, builder, true);
  if (res == PARSE_ERROR) return PARSE_ERROR;
  if (res == PARSE_OK) {
    *textp = text;
    return PARSE_OK;
  }
  
  res = parse_semicolon_statement(&text, builder);
  if (res == PARSE_ERROR) return PARSE_ERROR;
  if (res == PARSE_OK) {
    if (!eat_string(&text, ";")) {
      log_parser_error(text, "';' expected after statement");
      return PARSE_ERROR;
    }
    *textp = text;
    return PARSE_OK;
  }
  log_parser_error(text, "unknown statement");
  return PARSE_ERROR;
}

static ParseResult parse_block(char **textp, FunctionBuilder *builder, bool force_brackets) {
  char *text = *textp;
  
  int scope_backup = begin_lex_scope(builder);
  ParseResult res;
  
  if (eat_string(&text, "{")) {
    while (!eat_string(&text, "}")) {
      res = parse_statement(&text, builder);
      if (res == PARSE_ERROR) return res;
      assert(res == PARSE_OK);
    }
  } else {
    if (force_brackets) return PARSE_NONE;
    res = parse_statement(&text, builder);
    if (res == PARSE_ERROR) return res;
    assert(res == PARSE_OK);
  }
  
  *textp = text;
  end_lex_scope(builder, scope_backup);
  return PARSE_OK;
}

static ParseResult parse_function_expr(char **textp, UserFunction **uf_p) {
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
  FileRange *fnframe_range = alloc_and_record_start(text);
  if (!eat_string(&text, "(")) {
    log_parser_error(text, "opening paren for parameter list expected");
    free(fnframe_range);
    return PARSE_ERROR;
  }
  
  char **arg_list_ptr = NULL, arg_list_len = 0;
  char **type_constraints_ptr = NULL;
  bool variadic_tail = false;
  while (!eat_string(&text, ")")) {
    if (arg_list_len && !eat_string(&text, ",")) {
      log_parser_error(text, "comma expected");
      free(fnframe_range);
      return PARSE_ERROR;
    }
    if (eat_string(&text, "...")) {
      if (!eat_string(&text, ")")) {
        log_parser_error(text, "variadic marker must be the last argument to the function");
        free(fnframe_range);
        return PARSE_ERROR;
      }
      variadic_tail = true;
      break;
    }
    char *arg = parse_identifier(&text);
    if (!arg) {
      log_parser_error(text, "identifier expected");
      free(fnframe_range);
      return PARSE_ERROR;
    }
    char *constraint = NULL;
    if (eat_string(&text, ":")) {
      constraint = parse_identifier(&text);
      if (!constraint) {
        log_parser_error(text, "type constraint identifier expected");
        free(fnframe_range);
        return PARSE_ERROR;
      }
    }
    arg_list_ptr = realloc(arg_list_ptr, sizeof(char*) * ++arg_list_len);
    type_constraints_ptr = realloc(type_constraints_ptr, sizeof(char*) * arg_list_len);
    type_constraints_ptr[arg_list_len - 1] = constraint;
    arg_list_ptr[arg_list_len - 1] = arg;
  }
  record_end(text, fnframe_range);
  
  *textp = text;
  
  FunctionBuilder *builder = calloc(sizeof(FunctionBuilder), 1);
  builder->arglist_ptr = arg_list_ptr;
  builder->arglist_len = arg_list_len;
  builder->variadic_tail = variadic_tail;
  builder->slot_base = 2 + arg_list_len;
  builder->name = fun_name;
  builder->block_terminated = true;
  
  // generate lexical scope, initialize with parameters
  new_block(builder);
  use_range_start(builder, fnframe_range);
  builder->scope = 1;
  
  // look up constraints upfront (simplifies IR)
  int *constraint_slots = malloc(sizeof(int) * arg_list_len);
  for (int i = 0; i < arg_list_len; ++i) {
    constraint_slots[i] = -1;
    if (type_constraints_ptr[i]) {
      int type_name = addinstr_alloc_string_object(builder, type_constraints_ptr[i]);
      constraint_slots[i] = addinstr_access(builder, builder->scope, type_name);
    }
  }
  
  int *argname_slots = malloc(sizeof(int) * arg_list_len);
  builder->scope = addinstr_alloc_object(builder, builder->scope);
  for (int i = 0; i < arg_list_len; ++i) {
    argname_slots[i] = addinstr_alloc_string_object(builder, arg_list_ptr[i]);
    addinstr_assign(builder, builder->scope, argname_slots[i], 2 + i, ASSIGN_PLAIN);
  }
  addinstr_close_object(builder, builder->scope);
  for (int i = 0; i < arg_list_len; ++i) {
    if (type_constraints_ptr[i]) {
      addinstr_set_constraint(builder, builder->scope, argname_slots[i], constraint_slots[i]);
    }
  }
  use_range_end(builder, fnframe_range);
  free(constraint_slots);
  free(argname_slots);
  
  ParseResult res = parse_block(textp, builder, true);
  if (res == PARSE_ERROR) {
    free(fnframe_range);
    return PARSE_ERROR;
  }
  assert(res == PARSE_OK);
  
  use_range_start(builder, fnframe_range);
  terminate(builder);
  use_range_end(builder, fnframe_range);
  
  *uf_p = optimize(build_function(builder));
  return PARSE_OK;
}

ParseResult parse_module(char **textp, UserFunction **uf_p) {
  FunctionBuilder *builder = calloc(sizeof(FunctionBuilder), 1);
  builder->slot_base = 2;
  builder->name = NULL;
  builder->block_terminated = true;
  
  FileRange *modrange = alloc_and_record_start(*textp); // capture "module" statement if ever added
  record_end(*textp, modrange);
  
  use_range_start(builder, modrange);
  new_block(builder);
  builder->scope = 1;
  use_range_end(builder, modrange);
  
  if (eat_string(textp, "#!")) {
    while ((*textp)[0] && (*textp)[0] != '\n') (*textp)++;
    if ((*textp)[0] != '\n') {
      log_parser_error(*textp, "Hashbang found but no terminating newline!");
      return PARSE_ERROR;
    }
    (*textp)++;
  }
  
  while (true) {
    eat_filler(textp);
    if (!**textp) break;
    ParseResult res = parse_statement(textp, builder);
    if (res == PARSE_ERROR) return res;
    assert(res == PARSE_OK);
  }
  use_range_start(builder, modrange);
  addinstr_return(builder, builder->scope);
  use_range_end(builder, modrange);
  
  *uf_p = optimize(build_function(builder));
  return PARSE_OK;
}
