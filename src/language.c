#include "language.h"

#include <stdio.h>

#include <rdparse/parser.h>
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
  Slot base;
  Slot key;
  RefMode mode;
  FileRange *range;
  // some expressions such as variable definitions create accesses as side effect
  // those are
  bool safe_to_discard;
} RefValue;

static ParseResult parse_function_expr(char **textp, FunctionBuilder *pbuilder, UserFunction **uf);

static RefValue ref_simple(Slot slot) {
  return (RefValue) { slot, (Slot) { .index = -1 }, REFMODE_NONE, NULL };
}

static Slot ref_access(FunctionBuilder *builder, RefValue rv) {
  if (!builder) return (Slot) {0}; // happens when speculatively parsing
  if (slot_index(rv.key) != -1) {
    bool use_range = builder->current_range == NULL;
    if (use_range) use_range_start(builder, rv.range);
    Slot res = addinstr_access(builder, rv.base, rv.key);
    if (use_range) use_range_end(builder, rv.range);
    return res;
  }
  return rv.base;
}

static void ref_assign(FunctionBuilder *builder, RefValue rv, Slot value) {
  if (!builder) return;
  assert(slot_index(rv.key) != -1);
  addinstr_assign(builder, rv.base, rv.key, value, ASSIGN_PLAIN);
}

static void ref_assign_existing(FunctionBuilder *builder, RefValue rv, Slot value) {
  if (!builder) return;
  assert(rv.key.index != -1);
  addinstr_assign(builder, rv.base, rv.key, value, ASSIGN_EXISTING);
}

static void ref_assign_shadowing(FunctionBuilder *builder, RefValue rv, Slot value) {
  if (!builder) return;
  assert(rv.key.index != -1);
  addinstr_assign(builder, rv.base, rv.key, value, ASSIGN_SHADOWING);
}

// note: lexical scopes don't create new vm scopes, variable declarations do
// however, lexical scopes reset the "active scope" at the end
// (this is important, because it allows us to close the active
//  scope immediately after the variable declaration, allowing optimization later)

// IMPORTANT: because this is pure, you do NOT need to call end_lex_scope before returning errors!
static Slot begin_lex_scope(FunctionBuilder *builder) {
  return builder->scope;
}

static void end_lex_scope(FunctionBuilder *builder, Slot backup) {
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

static ParseResult parse_expr(char **textp, FunctionBuilder *builder, RefValue *rv);
static ParseResult parse_expr_oper(char **textp, FunctionBuilder *builder, int level, RefValue *rv);
static ParseResult parse_expr_base(char **textp, FunctionBuilder *builder, RefValue *rv);
static ParseResult parse_expr_continuation(char **textp, FunctionBuilder *builder, RefValue *rv, FileRange *expr_range);
static ParseResult parse_cond_prop_access(char **textp, FunctionBuilder *builder, RefValue *rv, FileRange *expr_range);
static ParseResult parse_vardecl_expr(char **textp, FunctionBuilder *builder, FileRange *var_range, bool isconst, RefValue *var_value);

static RefValue get_scope(FunctionBuilder *builder, char *name) {
  if (!builder) return (RefValue) { .base = {0}, .key = {0}, .mode = REFMODE_VARIABLE };
  assert(builder->current_range);
  Slot name_slot = addinstr_alloc_string_object(builder, name);
  return (RefValue) { .base = builder->scope, .key = name_slot, .mode = REFMODE_VARIABLE, .range = builder->current_range};
}

static ParseResult parse_object_literal_body(char **textp, FunctionBuilder *builder, Slot obj_slot) {
  char *text = *textp;
  while (!eat_string(&text, "}")) {
    
    FileRange *add_entry_range = alloc_and_record_start(text);
    
    bool is_method = false;
    if (eat_keyword(&text, "function") || (eat_keyword(&text, "method") && (is_method = true))) {
      char *ident_name = parse_identifier(&text);
      if (!ident_name) {
        log_parser_error(text, "object function must have name!");
        free(add_entry_range);
        return PARSE_ERROR;
      }
      record_end(text, add_entry_range);
      
      builder->hints.fun_name_hint_pos = text;
      builder->hints.fun_name_hint = ident_name;
      
      UserFunction *fn;
      ParseResult res = parse_function_expr(&text, builder, &fn);
      fn->is_method = is_method;
      if (res == PARSE_ERROR) return res;
      if (res == PARSE_NONE) {
        log_parser_error(text, "function expected");
        return PARSE_ERROR;
      }
      assert(res == PARSE_OK);
      if (builder) {
        use_range_start(builder, add_entry_range);
        Slot fn_slot = addinstr_alloc_closure_object(builder, fn);
        Slot key_slot = addinstr_alloc_string_object(builder, ident_name);
        addinstr_assign(builder, obj_slot, key_slot, fn_slot, ASSIGN_PLAIN);
        use_range_end(builder, add_entry_range);
      }
      continue;
    }
    
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
    
    FileRange *define_constraint = alloc_and_record_start(text);
    Slot constraint_slot = (Slot) { .index = -1 };
    if (eat_string(&text, ":")) {
      RefValue rv;
      record_end(text, define_constraint);
      ParseResult res = parse_expr_base(&text, builder, &rv);
      if (res == PARSE_ERROR) return PARSE_ERROR;
      if (res == PARSE_NONE) {
        log_parser_error(text, "missing type constraint");
        return PARSE_ERROR;
      }
      assert(res == PARSE_OK);
      constraint_slot = ref_access(builder, rv);
    }
    
    RefValue value;
    if (eat_string(&text, "=")) {
      ParseResult res = parse_expr(&text, builder, &value);
      if (res == PARSE_ERROR) return res;
      assert(res == PARSE_OK);
    } else {
      value = ref_simple((Slot) {0}); // init to null
    }
    
    
    if (!eat_string(&text, ";")) {
      log_parser_error(text, "object literal entry requires terminating semicolon");
      return PARSE_ERROR;
    }
    
    if (builder) {
      Slot value_slot = ref_access(builder, value);
      
      use_range_start(builder, add_entry_range);
      Slot key_slot = addinstr_alloc_string_object(builder, key_name);
      addinstr_assign(builder, obj_slot, key_slot, value_slot, ASSIGN_PLAIN);
      use_range_end(builder, add_entry_range);
      
      if (constraint_slot.index != -1) {
        use_range_start(builder, define_constraint);
        addinstr_set_constraint(builder, obj_slot, key_slot, constraint_slot);
        use_range_end(builder, define_constraint);
      }
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
  
  Slot obj_slot = {0};
  use_range_start(builder, range);
  if (builder) obj_slot = addinstr_alloc_object(builder, (Slot) {0});
  use_range_end(builder, range);
  *textp = text;
  *rv_p = ref_simple(obj_slot);
  
  ParseResult res = parse_object_literal_body(textp, builder, obj_slot);
  record_end(text, range);
  return res;
}

static ParseResult parse_array_literal_body(char **textp, FunctionBuilder *builder, Slot obj_slot, FileRange *range) {
  char *text = *textp;
  RefValue *values_ptr = NULL; int values_len = 0;
  while (!eat_string(&text, "]")) {
    RefValue value;
    ParseResult res = parse_expr(&text, builder, &value);
    if (res == PARSE_ERROR) return res;
    assert(res == PARSE_OK);
    
    if (builder) {
      values_ptr = realloc(values_ptr, sizeof(RefValue) * ++values_len);
      values_ptr[values_len - 1] = value;
    }
    if (eat_string(&text, ",")) continue;
    if (eat_string(&text, "]")) break;
    log_parser_error(text, "expected comma or closing square bracket");
    return PARSE_ERROR;
  }
  *textp = text;
  if (builder) {
    use_range_start(builder, range);
    Slot keyslot1 = addinstr_alloc_string_object(builder, "resize");
    Slot keyslot2 = addinstr_alloc_string_object(builder, "[]=");
    Slot resizefn = ref_access(builder, (RefValue) {obj_slot, keyslot1, REFMODE_OBJECT, range});
    Slot assignfn = ref_access(builder, (RefValue) {obj_slot, keyslot2, REFMODE_OBJECT, range});
    Slot newsize_slot = addinstr_alloc_int_object(builder, values_len);
    obj_slot = addinstr_call1(builder, resizefn, obj_slot, newsize_slot);
    for (int i = 0; i < values_len; ++i) {
      Slot index_slot = addinstr_alloc_int_object(builder, i);
      use_range_end(builder, range);
      Slot value_slot = ref_access(builder, values_ptr[i]);
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
  
  Slot obj_slot = {0};
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
  
  float f_value;
  if (parse_float(&text, &f_value)) {
    *textp = text;
    record_end(text, range);
    Slot slot = {0};
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
    Slot slot = {0};
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
    Slot slot = {0};
    if (res == PARSE_OK) {
      *textp = text;
      record_end(text, range);
      if (builder) {
        use_range_start(builder, range);
        slot = addinstr_alloc_string_object(builder, t_value);
        use_range_end(builder, range);
        builder->hints.string_literal_hint_slot = slot;
        builder->hints.string_literal_hint = t_value;
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
    
    res = parse_expr(&text, builder, rv);
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
    
    if (builder && *textp == builder->hints.fun_name_hint_pos) {
      builder->hints.fun_name_hint_pos = text; // advance
    }
    
    UserFunction *fn;
    ParseResult res = parse_function_expr(&text, builder, &fn);
    if (res == PARSE_ERROR) return res;
    if (res == PARSE_NONE) { text = *textp; record_start(text, range); }
    else {
      assert(res == PARSE_OK);
      *textp = text;
      
      fn->is_method = is_method;
      
      Slot slot = {0};
      if (builder) {
        use_range_start(builder, range); // count the closure allocation under 'function'
        slot = addinstr_alloc_closure_object(builder, fn);
        use_range_end(builder, range);
      }
      *rv = ref_simple(slot);
      return PARSE_OK;
    }
  }
  
  if (eat_keyword(&text, "new")) {
    record_end(text, range);
    
    RefValue parent_var;
    ParseResult res = parse_expr(&text, builder, &parent_var);
    if (res == PARSE_ERROR) return res;
    assert(res == PARSE_OK);
    
    Slot parent_slot = ref_access(builder, parent_var);
    Slot obj_slot = {0};
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
  
  char *ident_name = parse_identifier(&text);
  if (ident_name) {
    *textp = text;
    record_end(text, range);
    use_range_start(builder, range);
    *rv = get_scope(builder, ident_name);
    use_range_end(builder, range);
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
  *expr_range = *expr_range_dyn; // copy, since it's updated after each call segment
  
  if (!eat_string(&text, "(")) {
    free(call_range);
    return PARSE_NONE;
  }
  
  Slot *args_ptr = NULL; int args_len = 0;
  
  while (!eat_string(&text, ")")) {
    if (args_len && !eat_string(&text, ",")) {
      log_parser_error(text, "comma expected for call");
      return PARSE_ERROR;
    }
    RefValue arg;
    ParseResult res = parse_expr(&text, builder, &arg);
    if (res == PARSE_ERROR) return res;
    assert(res == PARSE_OK);
    
    args_ptr = realloc(args_ptr, sizeof(Slot) * ++args_len);
    args_ptr[args_len - 1] = ref_access(builder, arg);
  }
  
  record_end(text, call_range);
  
  *textp = text;
  if (builder) {
    Slot this_slot = {0};
    // scopes are not a valid form of "this"
    if (expr->key.index) this_slot = expr->base;
    
    Slot expr_slot = ref_access(builder, *expr);
    
    // use_range_start(builder, call_range);
    use_range_start(builder, expr_range);
    *expr = ref_simple(addinstr_call(builder, expr_slot, this_slot, args_ptr, args_len));
    // use_range_end(builder, call_range);
    use_range_end(builder, expr_range);
  } else *expr = ref_simple((Slot) {0});
  return PARSE_OK;
}

static ParseResult parse_cond_cont_call(char **textp, FunctionBuilder *builder, RefValue *rv, FileRange *expr_range) {
  char *text = *textp;
  
  char *text2 = text;
  if (!eat_string(&text2, "?") || !eat_string(&text2, "(")) {
    return PARSE_NONE;
  }
  if (!eat_string(&text, "?")) abort(); // ... what
  // text is now at (, which means we can reuse regular parse_expr_continuation
  
  int start_blk, call_blk, end_blk;
  int branch_start_call, branch_start_end;
  if (builder) {
    Slot rv_slot = ref_access(builder, *rv);
    
    start_blk = get_block(builder);
    
    use_range_start(builder, expr_range);
    
    rv_slot = addinstr_test(builder, rv_slot);
    addinstr_test_branch(builder, rv_slot, &branch_start_call, &branch_start_end);
    use_range_end(builder, expr_range);
    
    call_blk = new_block(builder);
    set_int_var(builder, branch_start_call, call_blk);
    
  }
  
  ParseResult res = parse_expr_continuation(&text, builder, rv, expr_range);
  if (res == PARSE_ERROR) return res;
  assert(res == PARSE_OK);
  
  if (builder) {
    use_range_start(builder, expr_range);
    Slot rv_slot = ref_access(builder, *rv);
    
    int branch_call_end;
    addinstr_branch(builder, &branch_call_end);
    
    end_blk = new_block(builder);
    set_int_var(builder, branch_start_end, end_blk);
    set_int_var(builder, branch_call_end, end_blk);
    
    rv_slot = addinstr_phi(builder, start_blk, (Slot) {0}, call_blk, rv_slot);
    use_range_end(builder, expr_range);
    
    *rv = ref_simple(rv_slot);
  } else *rv = ref_simple((Slot) {0});
  *textp = text;
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
  ParseResult res = parse_expr(&text, builder, &key);
  if (res == PARSE_ERROR) return res;
  assert(res == PARSE_OK);
  
  if (!eat_string(&text, "]")) {
    log_parser_error(*textp, "closing ']' expected");
    free(access_range);
    return PARSE_ERROR;
  }
  record_end(text, access_range);
  
  *textp = text;
  
  Slot key_slot = ref_access(builder, key);
  Slot expr_slot = ref_access(builder, *expr);
  
  if (builder && key_slot.index == builder->hints.string_literal_hint_slot.index) {
    builder->hints.fun_name_hint_pos = text;
    builder->hints.fun_name_hint = builder->hints.string_literal_hint;
  }
  
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
  
  Slot expr_slot = {0};
  Slot key_slot = {0};
  if (builder) expr_slot = ref_access(builder, *expr);
  use_range_start(builder, prop_range);
  if (builder) key_slot = addinstr_alloc_string_object(builder, keyname);
  *expr = (RefValue) {expr_slot, key_slot, REFMODE_OBJECT, prop_range};
  use_range_end(builder, prop_range);
  
  return PARSE_OK;
}

static ParseResult parse_cond_prop_access(char **textp, FunctionBuilder *builder, RefValue *rv, FileRange *expr_range) {
  char *text = *textp;
  
  FileRange *cprop_range = alloc_and_record_start(text);
  if (!eat_string(&text, "?") || !eat_string(&text, ".")) {
    free(cprop_range);
    return PARSE_NONE;
  }
  
  char *keyname = parse_identifier(&text);
  record_end(text, cprop_range);
  *textp = text;
  
  Slot lhs_slot = (Slot) {0};
  if (builder) lhs_slot = ref_access(builder, *rv);
  
  use_range_start(builder, cprop_range);
  
  // test if lhs is null, if true null, else test if rhs is in lhs, if false null, else continue
  int start_blk, lhs_nonnull_blk, rhs_in_lhs_blk, rhs_in_lhs_end_blk, end1_blk, end2_blk;
  if (builder) start_blk = get_block(builder);
  
  int branch_start_lhs_nonnull, branch_start_end1;
  if (builder) {
    Slot lhs_test = addinstr_test(builder, lhs_slot);
    addinstr_test_branch(builder, lhs_test, &branch_start_lhs_nonnull, &branch_start_end1);
    lhs_nonnull_blk = new_block(builder);
    set_int_var(builder, branch_start_lhs_nonnull, lhs_nonnull_blk);
  }
  
  Slot key_slot = {0};
  Slot key_in_slot = {0};
  int branch_lhs_nonnull_rhs_in_lhs, branch_lhs_nonnull_end2;
  if (builder) {
    key_slot = addinstr_alloc_string_object(builder, keyname);
    key_in_slot = addinstr_key_in_obj(builder, key_slot, lhs_slot);
    
    key_in_slot = addinstr_test(builder, key_in_slot);
    addinstr_test_branch(builder, key_in_slot, &branch_lhs_nonnull_rhs_in_lhs, &branch_lhs_nonnull_end2);
    
    rhs_in_lhs_blk = new_block(builder);
    set_int_var(builder, branch_lhs_nonnull_rhs_in_lhs, rhs_in_lhs_blk);
    
    use_range_end(builder, cprop_range);
  }
  
  *rv = (RefValue) {lhs_slot, key_slot, REFMODE_OBJECT, cprop_range};
  
  ParseResult res = parse_expr_continuation(&text, builder, rv, expr_range);
  if (res == PARSE_ERROR) return res;
  assert(res == PARSE_OK);
  Slot res_slot = ref_access(builder, *rv);
  
  int branch_rhs_in_lhs_end2;
  if (builder) {
    rhs_in_lhs_end_blk = get_block(builder);
  
    use_range_start(builder, cprop_range);
    addinstr_branch(builder, &branch_rhs_in_lhs_end2);
    
    end2_blk = new_block(builder);
    set_int_var(builder, branch_lhs_nonnull_end2, end2_blk);
    set_int_var(builder, branch_rhs_in_lhs_end2, end2_blk);
    res_slot = addinstr_phi(builder, lhs_nonnull_blk, (Slot) { .index = 0 }, rhs_in_lhs_end_blk, res_slot);
  }
  
  int branch_end2_end1;
  if (builder) {
    addinstr_branch(builder, &branch_end2_end1);
    
    end1_blk = new_block(builder);
    set_int_var(builder, branch_end2_end1, end1_blk);
    set_int_var(builder, branch_start_end1, end1_blk);
    res_slot = addinstr_phi(builder, start_blk, (Slot) { .index = 0 }, end2_blk, res_slot);
    use_range_end(builder, cprop_range);
  }
  
  *rv = ref_simple(res_slot);
  
  *textp = text;
  return PARSE_OK;
}

static ParseResult parse_cond_array_access(char **textp, FunctionBuilder *builder, RefValue *rv, FileRange *expr_range) {
  char *text = *textp;
  
  FileRange *carr_range = alloc_and_record_start(text);
  if (!eat_string(&text, "?") || !eat_string(&text, "[")) {
    free(carr_range);
    return PARSE_NONE;
  }
  
  RefValue key;
  ParseResult res = parse_expr(&text, builder, &key);
  if (res == PARSE_ERROR) return res;
  assert(res == PARSE_OK);
  
  if (!eat_string(&text, "]")) {
    log_parser_error(*textp, "closing ']' expected");
    free(carr_range);
    return PARSE_ERROR;
  }
  record_end(text, carr_range);
  
  *textp = text;
  
  Slot expr_slot = {0};
  if (builder) expr_slot = ref_access(builder, *rv);
  
  use_range_start(builder, carr_range);
  
  // test if lhs is null, if true null, else test if rhs is in lhs, if false null, else continue
  int start_blk, expr_nonnull_blk, idx_in_expr_blk, idx_in_expr_end_blk, end1_blk, end2_blk;
  if (builder) start_blk = get_block(builder);
  
  int branch_start_expr_nonnull, branch_start_end1;
  if (builder) {
    Slot expr_test = addinstr_test(builder, expr_slot);
    addinstr_test_branch(builder, expr_test, &branch_start_expr_nonnull, &branch_start_end1);
    expr_nonnull_blk = new_block(builder);
    set_int_var(builder, branch_start_expr_nonnull, expr_nonnull_blk);
  }
  
  Slot key_slot = {0};
  Slot key_in_slot = {0};
  int branch_expr_nonnull_key_in_expr, branch_key_nonnull_end2;
  if (builder) {
    key_slot = ref_access(builder, key);
    key_in_slot = addinstr_key_in_obj(builder, key_slot, expr_slot);
    
    key_in_slot = addinstr_test(builder, key_in_slot);
    addinstr_test_branch(builder, key_in_slot, &branch_expr_nonnull_key_in_expr, &branch_key_nonnull_end2);
    
    idx_in_expr_blk = new_block(builder);
    set_int_var(builder, branch_expr_nonnull_key_in_expr, idx_in_expr_blk);
    
    use_range_end(builder, carr_range);
  }
  
  *rv = (RefValue) {expr_slot, key_slot, REFMODE_OBJECT, carr_range};
  
  res = parse_expr_continuation(&text, builder, rv, expr_range);
  if (res == PARSE_ERROR) return res;
  assert(res == PARSE_OK);
  Slot res_slot = ref_access(builder, *rv);
  
  int branch_key_in_expr_end2;
  if (builder) {
    idx_in_expr_end_blk = get_block(builder);
  
    use_range_start(builder, carr_range);
    addinstr_branch(builder, &branch_key_in_expr_end2);
    
    end2_blk = new_block(builder);
    set_int_var(builder, branch_key_nonnull_end2, end2_blk);
    set_int_var(builder, branch_key_in_expr_end2, end2_blk);
    res_slot = addinstr_phi(builder, expr_nonnull_blk, (Slot) { .index = 0 }, idx_in_expr_end_blk, res_slot);
  }
  
  int branch_end2_end1;
  if (builder) {
    addinstr_branch(builder, &branch_end2_end1);
    
    end1_blk = new_block(builder);
    set_int_var(builder, branch_end2_end1, end1_blk);
    set_int_var(builder, branch_start_end1, end1_blk);
    res_slot = addinstr_phi(builder, start_blk, (Slot) { .index = 0 }, end2_blk, res_slot);
    use_range_end(builder, carr_range);
  }
  
  *rv = ref_simple(res_slot);
  
  *textp = text;
  return PARSE_OK;
}

void build_op(FunctionBuilder *builder, char *op, RefValue *res_rv, RefValue lhs_expr, RefValue rhs_expr, FileRange *range) {
  if (builder) {
    Slot lhs_value = ref_access(builder, lhs_expr);
    Slot rhs_value = ref_access(builder, rhs_expr);
    use_range_start(builder, range);
    Slot fn = addinstr_access(builder, lhs_value, addinstr_alloc_string_object(builder, op));
    *res_rv = ref_simple(addinstr_call1(builder, fn, lhs_value, rhs_value));
    use_range_end(builder, range);
  } else *res_rv = ref_simple((Slot) {0});
}

static bool assign_value(FunctionBuilder *builder, RefValue rv, Slot value, FileRange *assign_range) {
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
  
  Slot prev_slot = {0}, one_slot = {0};
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

static void negate(FunctionBuilder *builder, FileRange *range, RefValue *rv) {
  Slot negate_slot = {0};
  if (builder) {
    use_range_start(builder, range);
    
    Slot start_slot_t = addinstr_alloc_bool_object(builder, true);
    Slot start_rv_slot = ref_access(builder, *rv);
    int start_blk = get_block(builder);
    int start_br_true, start_br_false;
    start_rv_slot = addinstr_test(builder, start_rv_slot);
    addinstr_test_branch(builder, start_rv_slot, &start_br_true, &start_br_false);
    
    int true_blk = new_block(builder);
    set_int_var(builder, start_br_true, true_blk);
    Slot true_slot_f = addinstr_alloc_bool_object(builder, false);
    int true_br;
    addinstr_branch(builder, &true_br);
    
    int end_blk = new_block(builder);
    set_int_var(builder, start_br_false, end_blk);
    set_int_var(builder, true_br, end_blk);
    negate_slot = addinstr_phi(builder, start_blk, start_slot_t, true_blk, true_slot_f);
    use_range_end(builder, range);
  }
  *rv = ref_simple(negate_slot);
}

static ParseResult parse_expr_continuation(char **textp, FunctionBuilder *builder, RefValue *rv, FileRange *expr_range) {
  char *text = *textp;
  while (true) {
    record_end(text, expr_range);
    ParseResult res;
    if ((res = parse_cont_call(&text, builder, rv, expr_range)) == PARSE_OK) continue;
    if (res == PARSE_ERROR) return res;
    if ((res = parse_prop_access(&text, builder, rv)) == PARSE_OK) continue;
    if (res == PARSE_ERROR) return res;
    if ((res = parse_array_access(&text, builder, rv)) == PARSE_OK) continue;
    if (res == PARSE_ERROR) return res;
    if ((res = parse_postincdec(&text, builder, rv)) == PARSE_OK) continue;
    if (res == PARSE_ERROR) return res;
    if ((res = parse_cond_prop_access(&text, builder, rv, expr_range)) == PARSE_OK) break; // eats the tail
    if (res == PARSE_ERROR) return res;
    if ((res = parse_cond_cont_call(&text, builder, rv, expr_range)) == PARSE_OK) break; // eats the tail
    if (res == PARSE_ERROR) return res;
    if ((res = parse_cond_array_access(&text, builder, rv, expr_range)) == PARSE_OK) continue;
    if (res == PARSE_ERROR) return res;
    break;
  }
  
  *textp = text;
  return PARSE_OK;
}

static ParseResult parse_expr_base(char **textp, FunctionBuilder *builder, RefValue *rv) {
  char *text = *textp;
  
  FileRange *neg_range = alloc_and_record_start(text);
  if (eat_string(&text, "-")) {
    record_end(text, neg_range);
    ParseResult res = parse_expr_base(&text, builder, rv);
    if (res == PARSE_ERROR) return res;
    if (res != PARSE_OK) {
      log_parser_error(text, "expression to negate expected");
      return PARSE_ERROR;
    }
    *textp = text;
    
    use_range_start(builder, neg_range);
    Slot zero_slot = {0};
    if (builder) zero_slot = addinstr_alloc_int_object(builder, 0);
    use_range_end(builder, neg_range);
    RefValue zref = ref_simple(zero_slot);
    build_op(builder, "-", rv, zref, *rv, neg_range);
    return PARSE_OK;
  } else free(neg_range);
  
  FileRange *not_range = alloc_and_record_start(text);
  if (eat_string(&text, "!")) {
    ParseResult res = parse_expr_base(&text, builder, rv);
    if (res == PARSE_ERROR) return res;
    if (res != PARSE_OK) {
      log_parser_error(text, "expression to negate expected");
      return PARSE_ERROR;
    }
    *textp = text;
    record_end(text, not_range);
    negate(builder, not_range, rv);
    return PARSE_OK;
  } else free(not_range);
  
  FileRange *expr_range = alloc_and_record_start(text);
  ParseResult res = parse_expr_stem(&text, builder, rv);
  if (res == PARSE_ERROR) {
    free(expr_range);
    return PARSE_ERROR;
  }
  assert(res == PARSE_OK);
  
  res = parse_expr_continuation(&text, builder, rv, expr_range);
  if (res == PARSE_ERROR) {
    free(expr_range);
    return PARSE_ERROR;
  }
  assert(res == PARSE_OK);
  *textp = text;
  return res;
}

/*
 * 0: ||
 * 1: &&
 * 2: == != < > <= >=
 * 3: + -
 * 4: * / %
 * 5: |
 * 6: &
 * 7: in, is, instanceof
 */
static ParseResult parse_expr_oper(char **textp, FunctionBuilder *builder, int level, RefValue *rv) {
  char *text = *textp;
  
  ParseResult res = parse_expr_base(&text, builder, rv);
  if (res == PARSE_ERROR) return PARSE_ERROR;
  assert(res == PARSE_OK);
  
  RefValue rhs_expr;
  
  if (level > 6) { *textp = text; return PARSE_OK; }
  
  while (true) {
    char *text2 = text;
    
    bool negate_test = false;
    FileRange *negate_range = alloc_and_record_start(text2);
    if (eat_string(&text2, "!")) negate_test = true;
    record_end(text, negate_range);
    
    FileRange *range = alloc_and_record_start(text2);
    if (eat_keyword(&text2, "instanceof")) {
      text = text2;
      record_end(text, range);
      res = parse_expr_oper(&text, builder, 7, &rhs_expr);
      if (res == PARSE_ERROR) return PARSE_ERROR;
      assert(res == PARSE_OK);
      
      Slot lhs_slot = ref_access(builder, *rv);
      Slot rhs_slot = ref_access(builder, rhs_expr);
      
      use_range_start(builder, range);
      Slot result_slot = {0};
      if (builder) result_slot = addinstr_instanceof(builder, lhs_slot, rhs_slot);
      use_range_end(builder, range);
      *rv = ref_simple(result_slot);
      if (negate_test) negate(builder, negate_range, rv);
      continue;
    }
    if (eat_keyword(&text2, "in")) {
      text = text2;
      record_end(text, range);
      res = parse_expr_oper(&text, builder, 7, &rhs_expr);
      if (res == PARSE_ERROR) return PARSE_ERROR;
      assert(res == PARSE_OK);
      Slot key_slot = ref_access(builder, *rv);
      Slot obj_slot = ref_access(builder, rhs_expr);
      Slot in_slot = {0};
      use_range_start(builder, range);
      if (builder) in_slot = addinstr_key_in_obj(builder, key_slot, obj_slot);
      use_range_end(builder, range);
      *rv = ref_simple(in_slot);
      if (negate_test) negate(builder, negate_range, rv);
      continue;
    }
    if (eat_keyword(&text2, "is")) {
      text = text2;
      record_end(text, range);
      res = parse_expr_oper(&text, builder, 7, &rhs_expr);
      if (res == PARSE_ERROR) return PARSE_ERROR;
      assert(res == PARSE_OK);
      Slot obj1_slot = ref_access(builder, *rv);
      Slot obj2_slot = ref_access(builder, rhs_expr);
      Slot is_slot = {0};
      use_range_start(builder, range);
      if (builder) is_slot = addinstr_identical(builder, obj1_slot, obj2_slot);
      use_range_end(builder, range);
      *rv = ref_simple(is_slot);
      if (negate_test) negate(builder, negate_range, rv);
      continue;
    }
    free(range);
    break;
  }
  
  if (level > 6) { *textp = text; return PARSE_OK; }
  
  while (true) {
    FileRange *range = alloc_and_record_start(text);
    char *text2 = text;
    if (!eat_string(&text2, "&&") && eat_string(&text, "&")) {
      record_end(text, range);
      res = parse_expr_oper(&text, builder, 7, &rhs_expr);
      if (res == PARSE_ERROR) return PARSE_ERROR; assert(res == PARSE_OK);
      build_op(builder, "&", rv, *rv, rhs_expr, range);
      continue;
    }
    free(range);
    break;
  }
  
  if (level > 5) { *textp = text; return PARSE_OK; }
  
  while (true) {
    FileRange *range = alloc_and_record_start(text);
    char *text2 = text;
    if (!eat_string(&text2, "||") && eat_string(&text, "|")) {
      record_end(text, range);
      res = parse_expr_oper(&text, builder, 6, &rhs_expr);
      if (res == PARSE_ERROR) return PARSE_ERROR; assert(res == PARSE_OK);
      build_op(builder, "|", rv, *rv, rhs_expr, range);
      continue;
    }
    free(range);
    break;
  }
  
  if (level > 4) { *textp = text; return PARSE_OK; }
  
  while (true) {
    FileRange *range = alloc_and_record_start(text);
    if (eat_string(&text, "*")) {
      record_end(text, range);
      res = parse_expr_oper(&text, builder, 5, &rhs_expr);
      if (res == PARSE_ERROR) return PARSE_ERROR; assert(res == PARSE_OK);
      build_op(builder, "*", rv, *rv, rhs_expr, range);
      continue;
    }
    if (eat_string(&text, "/")) {
      record_end(text, range);
      res = parse_expr_oper(&text, builder, 5, &rhs_expr);
      if (res == PARSE_ERROR) return PARSE_ERROR; assert(res == PARSE_OK);
      build_op(builder, "/", rv, *rv, rhs_expr, range);
      continue;
    }
    if (eat_string(&text, "%")) {
      record_end(text, range);
      res = parse_expr_oper(&text, builder, 5, &rhs_expr);
      if (res == PARSE_ERROR) return PARSE_ERROR; assert(res == PARSE_OK);
      build_op(builder, "%", rv, *rv, rhs_expr, range);
      continue;
    }
    free(range);
    break;
  }
  
  if (level > 3) { *textp = text; return PARSE_OK; }
  
  while (true) {
    FileRange *range = alloc_and_record_start(text);
    if (eat_string(&text, "+")) {
      record_end(text, range);
      res = parse_expr_oper(&text, builder, 4, &rhs_expr);
      if (res == PARSE_ERROR) return PARSE_ERROR; assert(res == PARSE_OK);
      build_op(builder, "+", rv, *rv, rhs_expr, range);
      continue;
    }
    if (eat_string(&text, "-")) {
      record_end(text, range);
      res = parse_expr_oper(&text, builder, 4, &rhs_expr);
      if (res == PARSE_ERROR) return PARSE_ERROR; assert(res == PARSE_OK);
      build_op(builder, "-", rv, *rv, rhs_expr, range);
      continue;
    }
    free(range);
    break;
  }
  
  if (level > 2) { *textp = text; return PARSE_OK; }
  
  bool negate_expr = false;
  
  FileRange *range = alloc_and_record_start(text);
  if (eat_string(&text, "==")) {
    record_end(text, range);
    res = parse_expr_oper(&text, builder, 3, &rhs_expr);
    if (res == PARSE_ERROR) return PARSE_ERROR; assert(res == PARSE_OK);
    build_op(builder, "==", rv, *rv, rhs_expr, range);
  } else if (eat_string(&text, "!=")) {
    record_end(text, range);
    res = parse_expr_oper(&text, builder, 3, &rhs_expr);
    if (res == PARSE_ERROR) return PARSE_ERROR; assert(res == PARSE_OK);
    build_op(builder, "==", rv, *rv, rhs_expr, range);
    negate_expr = true;
  } else {
    if (eat_string(&text, "!")) {
      negate_expr = true;
    }
    if (eat_string(&text, "<=")) {
      record_end(text, range);
      res = parse_expr_oper(&text, builder, 3, &rhs_expr);
      if (res == PARSE_ERROR) return PARSE_ERROR; assert(res == PARSE_OK);
      build_op(builder, "<=", rv, *rv, rhs_expr, range);
    } else if (eat_string(&text, ">=")) {
      record_end(text, range);
      res = parse_expr_oper(&text, builder, 3, &rhs_expr);
      if (res == PARSE_ERROR) return PARSE_ERROR; assert(res == PARSE_OK);
      build_op(builder, ">=", rv, *rv, rhs_expr, range);
    } else if (eat_string(&text, "<")) {
      record_end(text, range);
      res = parse_expr_oper(&text, builder, 3, &rhs_expr);
      if (res == PARSE_ERROR) return PARSE_ERROR; assert(res == PARSE_OK);
      build_op(builder, "<", rv, *rv, rhs_expr, range);
    } else if (eat_string(&text, ">")) {
      record_end(text, range);
      res = parse_expr_oper(&text, builder, 3, &rhs_expr);
      if (res == PARSE_ERROR) return PARSE_ERROR; assert(res == PARSE_OK);
      build_op(builder, ">", rv, *rv, rhs_expr, range);
    } else if (negate_expr) {
      free(range);
      log_parser_error(text, "expected comparison operator");
      return PARSE_ERROR;
    }
  }
  
  if (negate_expr) {
    negate(builder, range, rv);
  }
  
  if (level > 1) { *textp = text; return PARSE_OK; }
  
  while (true) {
    FileRange *range = alloc_and_record_start(text);
    if (eat_string(&text, "&&")) {
      // a && b: b if truthy(a); otherwise a
      /*
       *  [1]
       * N | \ Y
       *   | [2]
       * 1 | / 2
       *  [φ]
       */
      record_end(text, range);
      
      int rhs_blk, lhs_br_false, lhs_blk;
      Slot lhs_slot;
      if (builder) {
        // short-circuiting evaluation
        lhs_slot = ref_access(builder, *rv);
        // if (lhs) {
        lhs_blk = get_block(builder);
        int lhs_br_true;
        use_range_start(builder, range);
        
        Slot lhs_test = addinstr_test(builder, lhs_slot);
        addinstr_test_branch(builder, lhs_test, &lhs_br_true, &lhs_br_false);
        
        rhs_blk = new_block(builder);
        set_int_var(builder, lhs_br_true, rhs_blk);
        use_range_end(builder, range);
      }
      res = parse_expr_oper(&text, builder, 2, &rhs_expr);
      if (res == PARSE_ERROR) return PARSE_ERROR;
      assert(res == PARSE_OK);
      if (builder) {
        Slot rhs_slot = ref_access(builder, rhs_expr);
        use_range_start(builder, range);
        rhs_blk = get_block(builder); // update because parse_expr_oper
        int rhs_br;
        addinstr_branch(builder, &rhs_br);
        
        // }
        int phi_blk = new_block(builder);
        set_int_var(builder, lhs_br_false, phi_blk);
        set_int_var(builder, rhs_br, phi_blk);
        Slot phi_slot = addinstr_phi(builder, lhs_blk, lhs_slot, rhs_blk, rhs_slot);
        use_range_end(builder, range);
        
        *rv = ref_simple(phi_slot);
      } else {
        *rv = ref_simple((Slot) { .index = 0 });
      }
      continue;
    }
    free(range);
    break;
  }
  
  if (level > 0) { *textp = text; return PARSE_OK; }
  
  while (true) {
    FileRange *range = alloc_and_record_start(text);
    if (eat_string(&text, "||")) {
      // a || b : a if truthy(a); otherwise b
      /*
       *  [1]
       * N | \ Y
       *   | [2]
       * 1 | / 2
       *  [φ]
       */
      record_end(text, range);
      
      int rhs_blk, lhs_br_true, lhs_blk;
      Slot lhs_slot;
      if (builder) {
        // short-circuiting evaluation
        lhs_slot = ref_access(builder, *rv);
        // if (!lhs) {
        lhs_blk = get_block(builder);
        int lhs_br_false;
        use_range_start(builder, range);
        
        Slot lhs_test = addinstr_test(builder, lhs_slot);
        addinstr_test_branch(builder, lhs_test, &lhs_br_true, &lhs_br_false);
        
        rhs_blk = new_block(builder);
        set_int_var(builder, lhs_br_false, rhs_blk);
        use_range_end(builder, range);
      }
      res = parse_expr_oper(&text, builder, 1, &rhs_expr);
      if (res == PARSE_ERROR) return PARSE_ERROR;
      assert(res == PARSE_OK);
      if (builder) {
        Slot rhs_slot = ref_access(builder, rhs_expr);
        use_range_start(builder, range);
        rhs_blk = get_block(builder); // parse_expr_oper may have changed block
        int rhs_br;
        addinstr_branch(builder, &rhs_br);
        
        // }
        int phi_blk = new_block(builder);
        set_int_var(builder, lhs_br_true, phi_blk);
        set_int_var(builder, rhs_br, phi_blk);
        Slot phi_slot = addinstr_phi(builder, lhs_blk, lhs_slot, rhs_blk, rhs_slot);
        use_range_end(builder, range);
        
        *rv = ref_simple(phi_slot);
      } else {
        *rv = ref_simple((Slot) {0});
      }
      continue;
    }
    free(range);
    break;
  }
  
  *textp = text;
  return PARSE_OK;
}

static ParseResult parse_expr(char **textp, FunctionBuilder *builder, RefValue *rv) {
  char *text = *textp;
  FileRange *keyword_range = alloc_and_record_start(text);
  if (eat_keyword(&text, "var")) {
    record_end(text, keyword_range);
    ParseResult res = parse_vardecl_expr(&text, builder, keyword_range, false, rv);
    if (res == PARSE_OK) { *textp = text; }
    return res;
  }
  if (eat_keyword(&text, "const")) {
    record_end(text, keyword_range);
    ParseResult res = parse_vardecl_expr(&text, builder, keyword_range, true, rv);
    if (res == PARSE_OK) { *textp = text; }
    return res;
  }
  free(keyword_range);
  return parse_expr_oper(textp, builder, 0, rv);
}

static ParseResult parse_block(char **textp, FunctionBuilder *builder, bool force_brackets);

static ParseResult parse_if(char **textp, FunctionBuilder *builder, FileRange *keywd_range) {
  char *text = *textp;
  if (!eat_string(&text, "(")) {
    log_parser_error(text, "if expected opening paren");
    return PARSE_ERROR;
  }
  Slot if_scope = begin_lex_scope(builder);
  RefValue test_expr;
  ParseResult res = parse_expr(&text, builder, &test_expr);
  if (res == PARSE_ERROR) return PARSE_ERROR;
  assert(res == PARSE_OK);
  Slot testslot = ref_access(builder, test_expr);
  if (!eat_string(&text, ")")) {
    log_parser_error(text, "if expected closing paren");
    return PARSE_ERROR;
  }
  int true_blk, false_blk, end_blk;
  use_range_start(builder, keywd_range);
  testslot = addinstr_test(builder, testslot);
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
    
    int end_blk2;
    use_range_start(builder, keywd_range);
    addinstr_branch(builder, &end_blk2);
    use_range_end(builder, keywd_range);
    int blk_idx = new_block(builder);
    set_int_var(builder, end_blk2, blk_idx);
    set_int_var(builder, end_blk, blk_idx);
  } else {
    set_int_var(builder, end_blk, false_blk_idx);
  }
  end_lex_scope(builder, if_scope);
  *textp = text;
  return PARSE_OK;
}

static ParseResult parse_while(char **textp, FunctionBuilder *builder, char *loop_label, FileRange *range) {
  char *text = *textp;
  if (!eat_string(&text, "(")) {
    log_parser_error(text, "'while' expected opening paren");
    return PARSE_ERROR;
  }
  
  LoopRecord *loop_record = open_loop(builder, loop_label);
  
  use_range_start(builder, range);
  int branch_test;
  addinstr_branch(builder, &branch_test);
  int test_blk = new_block(builder);
  set_int_var(builder, branch_test, test_blk);
  use_range_end(builder, range);
  
  int branch_test_loop, branch_test_end;
  
  Slot while_scope = begin_lex_scope(builder);
  RefValue test_expr;
  ParseResult res = parse_expr(&text, builder, &test_expr);
  if (res == PARSE_ERROR) return PARSE_ERROR;
  assert(res == PARSE_OK);
  
  Slot testslot = ref_access(builder, test_expr);
  if (!eat_string(&text, ")")) {
    log_parser_error(text, "'while' expected closing paren");
    return PARSE_ERROR;
  }
  use_range_start(builder, range);
  testslot = addinstr_test(builder, testslot);
  addinstr_test_branch(builder, testslot, &branch_test_loop, &branch_test_end);
  use_range_end(builder, range);
  
  int loop_blk = new_block(builder);
  set_int_var(builder, branch_test_loop, loop_blk);
  res = parse_block(&text, builder, false);
  if (res == PARSE_ERROR) return PARSE_ERROR;
  assert(res == PARSE_OK);
  
  use_range_start(builder, range);
  int test_blk2;
  addinstr_branch(builder, &test_blk2);
  use_range_end(builder, range);
  
  set_int_var(builder, test_blk2, test_blk);
  int end_blk = new_block(builder);
  set_int_var(builder, branch_test_end, end_blk);
  
  close_loop(builder, loop_record, end_blk, test_blk);
  
  end_lex_scope(builder, while_scope);
  
  *textp = text;
  return PARSE_OK;
}

static ParseResult parse_vardecl_expr(char **textp, FunctionBuilder *builder, FileRange *var_range, bool isconst, RefValue *var_value) {
  char *text = *textp;
  
  FileRange *alloc_var_name = alloc_and_record_start(text);
  char *varname = parse_identifier(&text);
  if (!varname) {
    log_parser_error(text, "invalid identifier for variable");
    return PARSE_ERROR;
  }
  record_end(text, alloc_var_name);
  
  FileRange *define_constraint = alloc_and_record_start(text);
  Slot constraint_slot = { .index = -1 };
  if (eat_string(&text, ":")) {
    RefValue rv;
    record_end(text, define_constraint);
    ParseResult res = parse_expr_base(&text, builder, &rv);
    if (res == PARSE_ERROR) return PARSE_ERROR;
    if (res == PARSE_NONE) {
      log_parser_error(text, "missing type constraint");
      return PARSE_ERROR;
    }
    assert(res == PARSE_OK);
    constraint_slot = ref_access(builder, rv);
  }
  
  // allocate the new scope upfront, so that the variable
  // is in scope for the value expression.
  // (this is important for recursion, ie. var foo = function() { foo(); }; )
  Slot value = {0}, varname_slot = {0}, var_scope = {0};
  if (builder) {
    use_range_start(builder, var_range);
    builder->scope = addinstr_alloc_object(builder, builder->scope);
    var_scope = builder->scope; // in case we later decide that expressions can open new scopes
    use_range_end(builder, var_range);
    
    use_range_start(builder, alloc_var_name);
    varname_slot = addinstr_alloc_string_object(builder, varname);
    addinstr_assign(builder, var_scope, varname_slot, (Slot) { .index = 0 }, ASSIGN_PLAIN);
    addinstr_close_object(builder, var_scope);
    use_range_end(builder, alloc_var_name);
  }
  
  FileRange *assign_value = alloc_and_record_start(text);
  
  if (!eat_string(&text, "=")) {
    free(assign_value);
    assign_value = alloc_var_name;
  } else {
    record_end(text, assign_value);
    RefValue rv;
    
    FileRange *calc_init_expr = alloc_and_record_start(text);
    ParseResult res = parse_expr(&text, builder, &rv);
    record_end(text, calc_init_expr);
    
    if (res == PARSE_ERROR) return res;
    assert(res == PARSE_OK);
    
    value = ref_access(builder, rv);
  }
  
  if (builder) {
    use_range_start(builder, assign_value);
    addinstr_assign(builder, var_scope, varname_slot, value, ASSIGN_EXISTING);
    use_range_end(builder, assign_value);
    
    if (constraint_slot.index != -1) {
      use_range_start(builder, define_constraint);
      addinstr_set_constraint(builder, var_scope, varname_slot, constraint_slot);
      use_range_end(builder, define_constraint);
    }
    
    use_range_start(builder, assign_value);
    if (isconst) addinstr_freeze_object(builder, var_scope);
    use_range_end(builder, assign_value);
    
    if (var_value) *var_value = (RefValue) {var_scope, varname_slot, REFMODE_VARIABLE, alloc_var_name, .safe_to_discard = true};
  } else {
    if (var_value) *var_value = (RefValue) {(Slot) {0}, (Slot) {0}, REFMODE_VARIABLE, 0, .safe_to_discard = true};
  }
  
  // var a, b;
  if (eat_string(&text, ",")) {
    *textp = text;
    return parse_vardecl_expr(textp, builder, var_range, isconst, var_value);
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
  if (res == PARSE_ERROR) return res;
  assert(res == PARSE_OK);
  char *text_before_assign = text;
  FileRange *assign_range = alloc_and_record_start(text);
  if (!eat_string(&text, assign_text)) abort(); // Internal inconsistency
  record_end(text, assign_range);
  
  if (builder && text_before_assign == builder->hints.fun_name_hint_pos) {
    builder->hints.fun_name_hint_pos = text; // advance
  }
  
  RefValue value_expr;
  res = parse_expr(&text, builder, &value_expr);
  if (res == PARSE_ERROR) {
    free(assign_range);
    return res;
  }
  assert(res == PARSE_OK);
  
  if (op) {
    build_op(builder, op, &value_expr, rv, value_expr, assign_range);
  }
  
  Slot value = ref_access(builder, value_expr);
  
  use_range_start(builder, assign_range);
  if (!assign_value(builder, rv, value, assign_range)) {
    log_parser_error(text, "cannot assign to non-reference expression!");
    return PARSE_ERROR;
  }
  
  *textp = text;
  return PARSE_OK;
}

static ParseResult parse_semicolon_statement(char **textp, FunctionBuilder *builder);

static ParseResult parse_for_in(char **textp, FunctionBuilder *builder, char *loop_label, FileRange *range) {
  char *text = *textp;
  if (!eat_string(&text, "(")) {
    return PARSE_NONE;
  }
  if (!eat_keyword(&text, "var")) return PARSE_NONE;
  char *var_name = parse_identifier(&text);
  char *key_name = NULL;
  if (!var_name) return PARSE_NONE;
  
  // key, value notation
  if (eat_string(&text, ",")) {
    key_name = var_name;
    var_name = parse_identifier(&text);
    if (!var_name) return PARSE_NONE;
  }
  
  if (!eat_keyword(&text, "in")) return PARSE_NONE;
  // we know it's an "in" loop now - can start erroring; this is where syntax diverges
  
  RefValue rv;
  ParseResult res = parse_expr(&text, builder, &rv);
  if (res == PARSE_ERROR) return PARSE_ERROR;
  if (res == PARSE_NONE) {
    log_parser_error(text, "'for-in' expected iterator expression");
    return PARSE_ERROR;
  }
  
  if (!eat_string(&text, ")")) {
    log_parser_error(text, "'for-in' expected closing paren");
    return PARSE_ERROR;
  }
  
  LoopRecord *loop_record = open_loop(builder, loop_label);
  
  Slot scope_backup;
  int test_blk, branch_test_body, branch_test_exit;
  if (builder) {
    scope_backup = begin_lex_scope(builder);
    
    use_range_start(builder, range);
    
    Slot obj_slot = ref_access(builder, rv);
    Slot iter_key = addinstr_alloc_string_object(builder, "iterator");
    Slot iterfn = ref_access(builder, (RefValue) {obj_slot, iter_key, REFMODE_OBJECT, range});
    Slot iter_slot = addinstr_call0(builder, iterfn, obj_slot);
    
    int branch_enter_test;
    
    addinstr_branch(builder, &branch_enter_test);
    
    test_blk = new_block(builder);
    set_int_var(builder, branch_enter_test, test_blk);
    
    Slot next_key = addinstr_alloc_string_object(builder, "next");
    Slot nextfn = ref_access(builder, (RefValue) {iter_slot, next_key, REFMODE_OBJECT, range});
    Slot pass_obj = addinstr_call0(builder, nextfn, iter_slot);
    Slot done_key = addinstr_alloc_string_object(builder, "done");
    Slot done_slot = ref_access(builder, (RefValue) { pass_obj, done_key, REFMODE_OBJECT, range});
    
    done_slot = addinstr_test(builder, done_slot);
    addinstr_test_branch(builder, done_slot, &branch_test_exit, &branch_test_body);
    
    int body_blk = new_block(builder);
    set_int_var(builder, branch_test_body, body_blk);
    
    builder->scope = addinstr_alloc_object(builder, builder->scope);
    Slot var_scope = builder->scope; // in case we later decide that expressions can open new scopes
    
    Slot varname_slot = addinstr_alloc_string_object(builder, var_name);
    Slot value_key = addinstr_alloc_string_object(builder, "value");
    Slot value_slot = ref_access(builder, (RefValue) { pass_obj, value_key, REFMODE_OBJECT, range});
    addinstr_assign(builder, var_scope, varname_slot, value_slot, ASSIGN_PLAIN);
    
    if (key_name) {
      Slot keyname_slot = addinstr_alloc_string_object(builder, key_name);
      Slot key_key = addinstr_alloc_string_object(builder, "key");
      Slot key_slot = ref_access(builder, (RefValue) { pass_obj, key_key, REFMODE_OBJECT, range});
      addinstr_assign(builder, var_scope, keyname_slot, key_slot, ASSIGN_PLAIN);
    }
    addinstr_close_object(builder, var_scope);
    
    use_range_end(builder, range);
  }
  
  res = parse_block(&text, builder, false);
  if (res == PARSE_ERROR) return PARSE_ERROR;
  assert(res == PARSE_OK);
  
  if (builder) {
    int branch_body_test;
    
    use_range_start(builder, range);
    addinstr_branch(builder, &branch_body_test);
    use_range_end(builder, range);
    set_int_var(builder, branch_body_test, test_blk);
    
    int exit_blk = new_block(builder);
    set_int_var(builder, branch_test_exit, exit_blk);
    
    close_loop(builder, loop_record, exit_blk, test_blk);
    end_lex_scope(builder, scope_backup);
  }
  
  *textp = text;
  
  return PARSE_OK;
}

static ParseResult parse_for(char **textp, FunctionBuilder *builder, char *loop_label, FileRange *range) {
  char *text = *textp;
  
  {
    ParseResult res = parse_for_in(textp, builder, loop_label, range);
    if (res == PARSE_ERROR) return res;
    if (res == PARSE_OK) return res;
  }
  
  LoopRecord *loop_record = open_loop(builder, loop_label);
  
  if (!eat_string(&text, "(")) {
    log_parser_error(text, "'for' expected opening paren");
    return PARSE_ERROR;
  }
  
  // variable is out of scope after the loop
  Slot scope_backup = begin_lex_scope(builder);
  
  FileRange *decl_range = alloc_and_record_start(text);
  if (eat_keyword(&text, "var")) {
    record_end(text, decl_range);
    ParseResult res = parse_vardecl_expr(&text, builder, decl_range, false, NULL);
    if (res == PARSE_ERROR) return res;
    assert(res == PARSE_OK);
  } else {
    ParseResult res = parse_assign(&text, builder);
    if (res == PARSE_ERROR) return res;
    if (res == PARSE_NONE) {
      log_parser_error(text, "'for' expected variable declaration or assignment");
      return PARSE_ERROR;
    }
  }
  
  if (!eat_string(&text, ";")) {
    log_parser_error(text, "'for' expected semicolon");
    return PARSE_ERROR;
  }
  
  use_range_start(builder, range);
  int test_blk;
  addinstr_branch(builder, &test_blk);
  int test_blk_idx = new_block(builder);
  set_int_var(builder, test_blk, test_blk_idx);
  use_range_end(builder, range);
  
  int loop_blk, branch_test_end;
  
  RefValue test_expr;
  ParseResult res = parse_expr(&text, builder, &test_expr);
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
  Slot testslot = ref_access(builder, test_expr);

  testslot = addinstr_test(builder, testslot);
  addinstr_test_branch(builder, testslot, &loop_blk, &branch_test_end);
  use_range_end(builder, range);
  
  // begin loop body
  set_int_var(builder, loop_blk, new_block(builder));
  res = parse_block(&text, builder, false);
  if (res == PARSE_ERROR) return PARSE_ERROR;
  assert(res == PARSE_OK);
  
  int branch_test_step;
  use_range_start(builder, range);
  addinstr_branch(builder, &branch_test_step);
  use_range_end(builder, range);
  
  int step_blk = new_block(builder);
  set_int_var(builder, branch_test_step, step_blk);
  res = parse_semicolon_statement(&text_step, builder);
  assert(res == PARSE_OK); // what? this already worked above...
  
  use_range_start(builder, range);
  int test_blk2;
  addinstr_branch(builder, &test_blk2);
  set_int_var(builder, test_blk2, test_blk_idx);
  
  use_range_end(builder, range);
  
  int end_blk = new_block(builder);
  set_int_var(builder, branch_test_end, end_blk);
  
  end_lex_scope(builder, scope_backup);
  
  close_loop(builder, loop_record, end_blk, step_blk);
  
  *textp = text;
  return PARSE_OK;
}

static ParseResult parse_return(char **textp, FunctionBuilder *builder, FileRange *keywd_range) {
  RefValue ret_value;
  char *text2 = *textp;
  Slot value;
  if (eat_string(&text2, ";")) {
    value = (Slot) { .index = 0 }; // null slot
  } else {
    ParseResult res = parse_expr(textp, builder, &ret_value);
    if (res == PARSE_ERROR) return res;
    assert(res == PARSE_OK);
    
    value = ref_access(builder, ret_value);
  }
  
  if (builder) {
    use_range_start(builder, keywd_range);
    addinstr_return(builder, value);
    new_block(builder);
    use_range_end(builder, keywd_range);
  }
  return PARSE_OK;
}

static ParseResult parse_fundecl(char **textp, FunctionBuilder *builder, FileRange *range, bool is_method) {
  // alloc scope for fun var
  use_range_start(builder, range);
  builder->scope = addinstr_alloc_object(builder, builder->scope);
  use_range_end(builder, range);
  
  UserFunction *fn;
  ParseResult res = parse_function_expr(textp, builder, &fn);
  if (res == PARSE_ERROR) return res;
  if (res == PARSE_NONE) {
    log_parser_error(*textp, "opening paren for parameter list expected");
    return PARSE_ERROR;
  }
  assert(res == PARSE_OK);
  fn->is_method = is_method;
  use_range_start(builder, range);
  Slot name_slot = addinstr_alloc_string_object(builder, fn->name);
  Slot slot = addinstr_alloc_closure_object(builder, fn);
  addinstr_assign(builder, builder->scope, name_slot, slot, ASSIGN_PLAIN);
  addinstr_close_object(builder, builder->scope);
  addinstr_freeze_object(builder, builder->scope);
  use_range_end(builder, range);
  return PARSE_OK;
}

static ParseResult parse_contbrk(char **textp, FunctionBuilder *builder, FileRange *range, bool is_break) {
  char *label_name = parse_identifier(textp);
  use_range_start(builder, range);
  char *error = loop_contbrk(builder, label_name, is_break);
  use_range_end(builder, range);
  if (error) {
    log_parser_error(*textp, error);
    return PARSE_ERROR;
  }
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
  else if (eat_keyword(&text, "continue")) {
    record_end(text, keyword_range);
    res = parse_contbrk(&text, builder, keyword_range, false);
  }
  else if (eat_keyword(&text, "break")) {
    record_end(text, keyword_range);
    res = parse_contbrk(&text, builder, keyword_range, true);
  }
  else {
    res = parse_assign(&text, builder);
    if (res == PARSE_NONE) {
      // expr as statement
      RefValue rv;
      res = parse_expr(&text, builder, &rv);
      if (res == PARSE_NONE) {
        return PARSE_NONE;
      }
      char *text2 = text;
      if (eat_string(&text2, ";")) { // otherwise it was just a syntax error after all
        if (rv.key.index != -1 && !rv.safe_to_discard) {
          log_parser_error(text, "property access discarded without effect");
          return PARSE_ERROR;
        }
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
  bool is_method = false;
  if (eat_keyword(&text, "function") || (eat_keyword(&text, "method") && (is_method = true))) {
    record_end(text, keyword_range);
    *textp = text;
    return parse_fundecl(textp, builder, keyword_range, is_method);
  }
  char *loop_label = NULL;
  bool for_with_label = false, while_with_label = false;
  {
    char *text2 = text;
    char *label = parse_identifier(&text2);
    if (label && eat_string(&text2, ":")) {
      loop_label = label;
      if (eat_keyword(&text2, "for")) {
        text = text2;
        for_with_label = true;
      } else if (eat_keyword(&text2, "while")) {
        text = text2;
        while_with_label = true;
      }
    }
  }
  if (while_with_label || eat_keyword(&text, "while")) {
    record_end(text, keyword_range);
    *textp = text;
    return parse_while(textp, builder, loop_label, keyword_range);
  }
  if (for_with_label || eat_keyword(&text, "for")) {
    record_end(text, keyword_range);
    *textp = text;
    return parse_for(textp, builder, loop_label, keyword_range);
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
  
  Slot scope_backup = begin_lex_scope(builder);
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

__thread int lambda_count = 0;

static ParseResult parse_function_expr(char **textp, FunctionBuilder *pbuilder, UserFunction **uf_p) {
  char *text = *textp;
  char *fun_name = parse_identifier(&text);
  char *fun_hint = fun_name;
  if (!fun_hint && pbuilder && pbuilder->hints.fun_name_hint_pos == text) {
    fun_hint = pbuilder->hints.fun_name_hint;
  }
  // TODO remove global use
  if (fun_hint) fun_hint = my_asprintf("%s <%i>", fun_hint, lambda_count++);
  else fun_hint = my_asprintf("Lambda <%i>", lambda_count++);
  /*
  ┌────────┐  ┌─────┐
  │function│═▷│  (  │
  └────────┘  └─────┘
               ║   ║
               ▽   ║
      ┌───┐  ┌───┐ ║
      │ , │◁═│arg│ ║
      │   │═▷│...│ ║
      └───┘  └───┘ ║
               ║   ║
               ▽   ▽
              ┌─────┐
              │  )  │
              └─────┘
  */
  FileRange *fn_range = alloc_and_record_start(text);
  FileRange *fnframe_range = alloc_and_record_start(text);
  if (!eat_string(&text, "(")) {
    // log_parser_error(text, "opening paren for parameter list expected");
    // free(fnframe_range);
    // return PARSE_ERROR;
    
    // "function" is an identifier as well; consider foo instanceof function
    return PARSE_NONE;
  }
  
  char **arg_list_ptr = NULL, arg_list_len = 0;
  char **type_constraints_ptr = NULL;
  bool variadic_tail = false;
  while (!eat_string(&text, ")")) {
    if (arg_list_len && !eat_string(&text, ",")) {
      log_parser_error(text, "comma expected for argument list");
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
    char *textpos = NULL;
    if (eat_string(&text, ":")) {
      textpos = text;
      RefValue constraint;
      ParseResult res = parse_expr_base(&text, NULL, &constraint);
      if (res == PARSE_ERROR) return PARSE_ERROR;
      if (res == PARSE_NONE) {
        log_parser_error(text, "missing type constraint");
        return PARSE_ERROR;
      }
      assert(res == PARSE_OK);
    }
    arg_list_ptr = realloc(arg_list_ptr, sizeof(char*) * ++arg_list_len);
    type_constraints_ptr = realloc(type_constraints_ptr, sizeof(char*) * arg_list_len);
    type_constraints_ptr[arg_list_len - 1] = textpos;
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
  builder->body.function_range = fnframe_range; // TODO use a better one
  
  // generate lexical scope, initialize with parameters
  new_block(builder);
  builder->scope = (Slot) { .index = 1 };
  
  // look up constraints upfront (simplifies IR)
  Slot *constraint_slots = malloc(sizeof(Slot) * arg_list_len);
  for (int i = 0; i < arg_list_len; ++i) {
    constraint_slots[i] = (Slot) { .index = -1 };
    if (type_constraints_ptr[i]) {
      RefValue constraint;
      char *text2 = type_constraints_ptr[i];
      ParseResult res = parse_expr_base(&text2, builder, &constraint);
      (void) res; assert(res == PARSE_OK);
      constraint_slots[i] = ref_access(builder, constraint);
    }
  }
  
  use_range_start(builder, fnframe_range);
  Slot *argname_slots = malloc(sizeof(Slot) * arg_list_len);
  builder->scope = addinstr_alloc_object(builder, builder->scope);
  for (int i = 0; i < arg_list_len; ++i) {
    argname_slots[i] = addinstr_alloc_string_object(builder, arg_list_ptr[i]);
    addinstr_assign(builder, builder->scope, argname_slots[i], (Slot) { .index = 2 + i }, ASSIGN_PLAIN);
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
  if (res == PARSE_NONE) {
    free(fnframe_range);
    log_parser_error(text, "could not parse function block");
    return PARSE_ERROR;
  }
  assert(res == PARSE_OK);
  
  record_end(*textp, fn_range);
  register_function((TextRange) { fn_range->text_from, fn_range->text_from + fn_range->text_len }, fun_hint);
  
  use_range_start(builder, fnframe_range);
  terminate(builder);
  use_range_end(builder, fnframe_range);
  
  *uf_p = optimize(build_function(builder));
  finalize(*uf_p);
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
  builder->scope = (Slot) { .index = 1 };
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
  
  *uf_p = build_function(builder);
  *uf_p = optimize(*uf_p);
  finalize(*uf_p);
  return PARSE_OK;
}
