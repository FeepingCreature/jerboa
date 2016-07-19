#include "vm/optimize.h"

#include <stdio.h>

#include "vm/builder.h"
#include "vm/cfg.h"
#include "gc.h"

// mark slots whose value is only
// used as parameter to other instructions and does not escape
// such as string keys
static void slot_is_primitive(UserFunction *uf, bool** slots_p) {
  *slots_p = malloc(sizeof(bool) * uf->slots);
  bool *slots = *slots_p;
  for (int i = 0; i < uf->slots; ++i) slots[i] = true; // default assumption
  
  for (int i = 0; i < uf->body.blocks_len; ++i) {
    InstrBlock *block = &uf->body.blocks_ptr[i];
    Instr *instr = block->instrs_ptr;
    while (instr != block->instrs_ptr_end) {
#define CASE(KEY, TY, VAR) instr = (Instr*) ((char*) instr + _stepsize); } break; \
        case KEY: { int _stepsize = sizeof(TY); TY *VAR = (TY*) instr; (void) VAR;
      switch (instr->type) {
        case INSTR_INVALID: { abort(); int _stepsize = -1;
          CASE(INSTR_GET_ROOT, GetRootInstr, get_root_instr)
          CASE(INSTR_ALLOC_OBJECT, AllocObjectInstr, alloc_obj_instr)
            slots[alloc_obj_instr->parent_slot] = false;
          CASE(INSTR_ALLOC_INT_OBJECT, AllocIntObjectInstr, alloc_int_obj_instr)
          CASE(INSTR_ALLOC_FLOAT_OBJECT, AllocFloatObjectInstr, alloc_float_obj_instr)
          CASE(INSTR_ALLOC_ARRAY_OBJECT, AllocArrayObjectInstr, alloc_array_obj_instr)
          CASE(INSTR_ALLOC_STRING_OBJECT, AllocStringObjectInstr, alloc_string_obj_instr)
          CASE(INSTR_ALLOC_CLOSURE_OBJECT, AllocClosureObjectInstr, alloc_closure_obj_instr)
          CASE(INSTR_CLOSE_OBJECT, CloseObjectInstr, close_obj_instr)
          CASE(INSTR_FREEZE_OBJECT, FreezeObjectInstr, freeze_obj_instr)
          CASE(INSTR_ACCESS, AccessInstr, access_instr)
            slots[access_instr->obj_slot] = false;
          CASE(INSTR_ASSIGN, AssignInstr, assign_instr)
            slots[assign_instr->obj_slot] = slots[assign_instr->value_slot] = false;
          // TODO inline key?
          CASE(INSTR_KEY_IN_OBJ, KeyInObjInstr, key_in_obj_instr)
          CASE(INSTR_SET_CONSTRAINT, SetConstraintInstr, scons_instr)
            slots[scons_instr->obj_slot] = slots[scons_instr->constraint_slot] = false;
          CASE(INSTR_CALL, CallInstr, call_instr)
            slots[call_instr->function_slot] = slots[call_instr->this_slot] = false;
            for (int i = 0; i < call_instr->args_length; ++i) {
              slots[call_instr->args_ptr[i]] = false;
            }
          CASE(INSTR_RETURN, ReturnInstr, return_instr)
            slots[return_instr->ret_slot] = false;
          CASE(INSTR_SAVE_RESULT, SaveResultInstr, save_result_instr)
          CASE(INSTR_BR, BranchInstr, branch_instr)
          CASE(INSTR_TESTBR, TestBranchInstr, test_branch_instr)
            slots[test_branch_instr->test_slot] = false;
          instr = (Instr*) ((char*) instr + _stepsize);
        } break;
        default: assert("Unhandled Instruction Type!" && false);
      }
#undef CASE
    }
  }
}

typedef struct {
  bool static_object;
  int parent_slot;
  
  int fields_len;
  char **names_ptr;
  Object **constraints_ptr;
  Instr **constraint_imposed_here_ptr;
  
  FileRange *belongs_to;
  int context_slot;
  Instr *after_object_decl;
} SlotIsStaticObjInfo;

#include "vm/dump.h"

// static object: allocated, assigned a few keys, and closed.
static void slot_is_static_object(UserFunction *uf, SlotIsStaticObjInfo **slots_p) {
  *slots_p = calloc(sizeof(SlotIsStaticObjInfo), uf->slots);
  
  for (int i = 0; i < uf->body.blocks_len; ++i) {
    InstrBlock *block = &uf->body.blocks_ptr[i];
    Instr *instr = block->instrs_ptr;
    Instr *instr_end = block->instrs_ptr_end;
    while (instr != instr_end) {
      // Instr *instr2 = instr;
      // dump_instr(&instr2);
      
      if (instr->type == INSTR_ALLOC_OBJECT) {
        AllocObjectInstr *alobi = (AllocObjectInstr*) instr;
        instr = (Instr*) (alobi + 1);
        bool failed = false;
        char **names_ptr = 0; int fields_len = 0;
        while (instr != instr_end) {
          if (instr->type == INSTR_ASSIGN_STRING_KEY) {
            AssignStringKeyInstr *aski = (AssignStringKeyInstr*) instr;
            if (aski->type != ASSIGN_PLAIN) { failed = true; break; }
            instr = (Instr*) (aski + 1);
            names_ptr = realloc(names_ptr, sizeof(char*) * ++fields_len);
            names_ptr[fields_len - 1] = aski->key;
          } else if (instr->type == INSTR_CLOSE_OBJECT) {
            break;
          } else {
            failed = true;
            // fprintf(stderr, "failed slot %i because %i\n", alobi->target_slot, instr->type);
            break;
          }
        }
        if (failed) {
          free(names_ptr);
          continue;
        }
        int target_slot = alobi->target_slot;
        (*slots_p)[target_slot].static_object = true;
        (*slots_p)[target_slot].parent_slot = alobi->parent_slot;
        (*slots_p)[target_slot].fields_len = fields_len;
        (*slots_p)[target_slot].names_ptr = names_ptr;
        (*slots_p)[target_slot].constraints_ptr = calloc(sizeof(Object*), fields_len);
        (*slots_p)[target_slot].constraint_imposed_here_ptr = calloc(sizeof(Instr*), fields_len);
        (*slots_p)[target_slot].belongs_to = instr->belongs_to;
        (*slots_p)[target_slot].context_slot = instr->context_slot;
        
        instr = (Instr*)((CloseObjectInstr*) instr + 1);
        (*slots_p)[target_slot].after_object_decl = instr;
        continue;
      }
      instr = (Instr*)((char*) instr + instr_size(instr));
    }
  }
}

static void copy_fn_stats(UserFunction *from, UserFunction *to) {
  to->slots = from->slots;
  to->refslots = from->refslots;
  to->arity = from->arity;
  to->name = from->name;
  to->is_method = from->is_method;
  to->variadic_tail = from->variadic_tail;
}

static UserFunction *redirect_predictable_lookup_misses(UserFunction *uf) {
  SlotIsStaticObjInfo *info;
  slot_is_static_object(uf, &info);
  
  FunctionBuilder *builder = calloc(sizeof(FunctionBuilder), 1);
  builder->slot_base = 1;
  builder->refslot_base = uf->refslots;
  builder->block_terminated = true;
  
  for (int i = 0; i < uf->body.blocks_len; ++i) {
    InstrBlock *block = &uf->body.blocks_ptr[i];
    new_block(builder);
    
    Instr *instr = block->instrs_ptr;
    while (instr != block->instrs_ptr_end) {
      AccessStringKeyInstr *aski = (AccessStringKeyInstr*) instr;
      if (instr->type == INSTR_ACCESS_STRING_KEY) {
        AccessStringKeyInstr *aski_new = malloc(sizeof(AccessStringKeyInstr));
        *aski_new = *aski;
        while (true) {
          int obj_slot = aski_new->obj_slot;
          if (!info[obj_slot].static_object) break;
          bool key_in_obj = false;
          for (int k = 0; k < info[obj_slot].fields_len; ++k) {
            char *objkey = info[obj_slot].names_ptr[k];
            if (strcmp(objkey, aski_new->key_ptr) == 0) {
              key_in_obj = true;
              break;
            }
          }
          if (key_in_obj) break;
          // since the key was not found in obj
          // we can know statically that the lookup will
          // not succeed at runtime either
          // (since the object is closed, its keys are statically known)
          // so instead look up in the (known) parent object from the start
          aski_new->obj_slot = info[obj_slot].parent_slot;
        }
        addinstr_like(builder, instr, sizeof(*aski_new), (Instr*) aski_new);
        instr = (Instr*) (aski + 1);
        continue;
      }
      
      addinstr_like(builder, instr, instr_size(instr), instr);
      instr = (Instr*) ((char*) instr + instr_size(instr));
    }
  }
  UserFunction *fn = build_function(builder);
  copy_fn_stats(uf, fn);
  return fn;
}

static UserFunction *access_vars_via_refslots(UserFunction *uf) {
  SlotIsStaticObjInfo *info;
  slot_is_static_object(uf, &info);
  
  int info_slots_len = 0;
  for (int i = 0; i < uf->slots; ++i) if (info[i].static_object) info_slots_len ++;
  int *info_slots_ptr = malloc(sizeof(int) * info_slots_len);
  bool *obj_refslots_initialized = calloc(sizeof(bool), uf->slots);
  int **ref_slots_ptr = calloc(sizeof(int*), uf->slots);
  {
    int k = 0;
    for (int i = 0; i < uf->slots; ++i) if (info[i].static_object) {
      info_slots_ptr[k++] = i;
      ref_slots_ptr[i] = malloc(sizeof(int) * info[i].fields_len);
    }
  }
  
  FunctionBuilder *builder = calloc(sizeof(FunctionBuilder), 1);
  builder->slot_base = 1;
  builder->refslot_base = uf->refslots;
  builder->block_terminated = true;
  
  // since the object accesses must dominate the object declaration,
  // the refslot accesses will also dominate the refslot declaration.
  
  for (int i = 0; i < uf->body.blocks_len; ++i) {
    InstrBlock *block = &uf->body.blocks_ptr[i];
    new_block(builder);
    
    Instr *instr = block->instrs_ptr;
    while (instr != block->instrs_ptr_end) {
      // O(n²) but nbd
      // TODO bdaa
      // TODO store block/instr index with after_object_decl, invert into array of arrays for instrs
      for (int k = 0; k < info_slots_len; ++k) {
        int slot = info_slots_ptr[k];
        if (instr == info[slot].after_object_decl) {
          use_range_start(builder, info[slot].belongs_to);
          builder->scope = info[slot].context_slot;
          for (int l = 0; l < info[slot].fields_len; ++l) {
            ref_slots_ptr[slot][l] = addinstr_def_refslot(builder, slot, info[slot].names_ptr[l]);
          }
          use_range_end(builder, info[slot].belongs_to);
          obj_refslots_initialized[slot] = true;
        }
      }
      
      if (instr->type == INSTR_ACCESS_STRING_KEY) {
        AccessStringKeyInstr *aski = (AccessStringKeyInstr*) instr;
        int obj_slot = aski->obj_slot;
        char *key = aski->key_ptr;
        if (info[obj_slot].static_object && obj_refslots_initialized[obj_slot]) {
          bool continue_outer = false;
          for (int k = 0; k < info[obj_slot].fields_len; ++k) {
            char *name = info[obj_slot].names_ptr[k];
            if (strcmp(key, name) == 0) {
              int refslot = ref_slots_ptr[obj_slot][k];
              use_range_start(builder, instr->belongs_to);
              builder->scope = instr->context_slot;
              addinstr_read_refslot(builder, refslot, aski->target_slot, my_asprintf("refslot reading '%s'", name));
              use_range_end(builder, instr->belongs_to);
              instr = (Instr*) (aski + 1);
              continue_outer = true;
              break;
            }
          }
          if (continue_outer) continue;
        }
      }
      
      if (instr->type == INSTR_ASSIGN_STRING_KEY) {
        AssignStringKeyInstr *aski = (AssignStringKeyInstr*) instr;
        int obj_slot = aski->obj_slot;
        char *key = aski->key;
        if (info[obj_slot].static_object && obj_refslots_initialized[obj_slot]) {
          bool continue_outer = false;
          for (int k = 0; k < info[obj_slot].fields_len; ++k) {
            char *name = info[obj_slot].names_ptr[k];
            if (strcmp(key, name) == 0) {
              int refslot = ref_slots_ptr[obj_slot][k];
              use_range_start(builder, instr->belongs_to);
              builder->scope = instr->context_slot;
              addinstr_write_refslot(builder, aski->value_slot, refslot, my_asprintf("refslot writing '%s'", name));
              use_range_end(builder, instr->belongs_to);
              instr = (Instr*) (aski + 1);
              continue_outer = true;
              break;
            }
          }
          if (continue_outer) continue;
        }
      }
      
      addinstr_like(builder, instr, instr_size(instr), instr);
      instr = (Instr*) ((char*) instr + instr_size(instr));
    }
  }
  UserFunction *fn = build_function(builder);
  int new_refslots = fn->refslots;
  copy_fn_stats(uf, fn);
  fn->refslots = new_refslots; // do update this
  
  free(info_slots_ptr);
  free(obj_refslots_initialized);
  for (int i = 0; i < uf->slots; ++i) {
    free(ref_slots_ptr[i]);
  }
  free(ref_slots_ptr);
  
  return fn;
}

static UserFunction *inline_primitive_accesses(UserFunction *uf, bool *prim_slot) {
  FunctionBuilder *builder = calloc(sizeof(FunctionBuilder), 1);
  builder->slot_base = 1;
  builder->refslot_base = uf->refslots;
  builder->block_terminated = true;
  
  char **slot_table_ptr = NULL;
  int slot_table_len = 0;
  
  for (int i = 0; i < uf->body.blocks_len; ++i) {
    InstrBlock *block = &uf->body.blocks_ptr[i];
    new_block(builder);
    
    Instr *instr = block->instrs_ptr;
    while (instr != block->instrs_ptr_end) {
      AllocStringObjectInstr *asoi = (AllocStringObjectInstr*) instr;
      AccessInstr *acci = (AccessInstr*) instr;
      AssignInstr *assi = (AssignInstr*) instr;
      SetConstraintInstr *sci = (SetConstraintInstr*) instr;
      if (instr->type == INSTR_ALLOC_STRING_OBJECT
        && prim_slot[asoi->target_slot] == true)
      {
        if (slot_table_len < asoi->target_slot + 1) {
          slot_table_ptr = realloc(slot_table_ptr, sizeof(char*) * (asoi->target_slot + 1));
          for (int i = slot_table_len; i < asoi->target_slot + 1; ++i) {
            slot_table_ptr[i] = NULL;
          }
          slot_table_len = asoi->target_slot + 1;
        }
        slot_table_ptr[asoi->target_slot] = asoi->value;
        instr = (Instr*)(asoi + 1);
        continue; // no need to add, we're fully inlining this
      }
      if (instr->type == INSTR_ACCESS
        && acci->key_slot < slot_table_len && slot_table_ptr[acci->key_slot] != NULL)
      {
        AccessStringKeyInstr *aski = malloc(sizeof(AccessStringKeyInstr));
        aski->base.type = INSTR_ACCESS_STRING_KEY;
        aski->obj_slot = acci->obj_slot;
        aski->target_slot = acci->target_slot;
        aski->key_ptr = slot_table_ptr[acci->key_slot];
        aski->key_len = strlen(aski->key_ptr);
        aski->key_hash = hash(aski->key_ptr, aski->key_len);
        addinstr_like(builder, instr, sizeof(*aski), (Instr*) aski);
        instr = (Instr*)(acci + 1);
        continue;
      }
      if (instr->type == INSTR_ASSIGN
        && assi->key_slot < slot_table_len && slot_table_ptr[assi->key_slot] != NULL)
      {
        AssignStringKeyInstr *aski = malloc(sizeof(AssignStringKeyInstr));
        aski->base.type = INSTR_ASSIGN_STRING_KEY;
        aski->obj_slot = assi->obj_slot;
        aski->value_slot = assi->value_slot;
        aski->key = slot_table_ptr[assi->key_slot];
        aski->type = assi->type;
        addinstr_like(builder, instr, sizeof(*aski), (Instr*) aski);
        instr = (Instr*)(assi + 1);
        continue;
      }
      if (instr->type == INSTR_SET_CONSTRAINT
        && sci->key_slot < slot_table_len && slot_table_ptr[sci->key_slot] != NULL)
      {
        SetConstraintStringKeyInstr scski;
        scski.base.type = INSTR_SET_CONSTRAINT_STRING_KEY;
        scski.base.belongs_to = NULL;
        scski.obj_slot = sci->obj_slot;
        scski.constraint_slot = sci->constraint_slot;
        scski.key_ptr = slot_table_ptr[sci->key_slot];
        scski.key_len = strlen(scski.key_ptr);
        addinstr_like(builder, instr, sizeof(scski), (Instr*) &scski);
        instr = (Instr*)(sci + 1);
        continue;
      }
      addinstr_like(builder, instr, instr_size(instr), instr);
      
      instr = (Instr*) ((char*) instr + instr_size(instr));
    }
  }
  UserFunction *fn = build_function(builder);
  copy_fn_stats(uf, fn);
  return fn;
}

#include "print.h"

// if lookup for "key" in "context" will always return the same value, return it.
Object *lookup_statically(Object *obj, char *key_ptr, int key_len, size_t hashv, bool *key_found_p) {
  *key_found_p = false;
  while (obj) {
    TableEntry *entry = table_lookup_with_hash(&obj->tbl, key_ptr, key_len, hashv);
    if (entry) {
      // hit, but the value might change later! bad!
      if (!(obj->flags & OBJ_FROZEN)) {
        // printf("hit for %.*s, but object wasn't frozen\n", key_len, key_ptr);
        return NULL;
      }
      *key_found_p = true;
      return entry->value;
    }
    // no hit, but ... 
    // if the object is not closed, somebody might
    // insert a different object of "key" later!
    // note that the current object just needs to be frozen,
    // because if it gets a hit, we won't be able to overwrite it.
    if (!(obj->flags & OBJ_CLOSED)) {
      // printf("hit for %.*s, but object wasn't closed\n", key_len, key_ptr);
      return NULL;
    }
    obj = obj->parent;
  }
  return NULL; // no hits.
}

UserFunction *inline_static_lookups_to_constants(VMState *state, UserFunction *uf, Object *context) {
  bool *object_known = calloc(sizeof(bool), uf->slots);
  Object **known_objects_table = calloc(sizeof(Object*), uf->slots);
  object_known[1] = true;
  known_objects_table[1] = context;
  for (int i = 0; i < uf->body.blocks_len; ++i) {
    InstrBlock *block = &uf->body.blocks_ptr[i];
    Instr *instr = block->instrs_ptr;
    Instr *instr_end = block->instrs_ptr_end;
    while (instr != instr_end) {
      if (instr->type == INSTR_ACCESS_STRING_KEY) {
        AccessStringKeyInstr *aski = (AccessStringKeyInstr*) instr;
        if (object_known[aski->obj_slot]) {
          Object *known_obj = known_objects_table[aski->obj_slot];
          bool key_found;
          Object *static_lookup = lookup_statically(known_obj,
                                                    aski->key_ptr, aski->key_len, aski->key_hash,
                                                    &key_found);
          if (key_found) {
            object_known[aski->target_slot] = true;
            known_objects_table[aski->target_slot] = static_lookup;
          }
        }
      }
      instr = (Instr*) ((char*) instr + instr_size(instr));
    }
  }
  
  FunctionBuilder *builder = calloc(sizeof(FunctionBuilder), 1);
  builder->slot_base = 1;
  builder->refslot_base = uf->refslots;
  builder->block_terminated = true;
  
  for (int i = 0; i < uf->body.blocks_len; ++i) {
    InstrBlock *block = &uf->body.blocks_ptr[i];
    new_block(builder);
    
    Instr *instr = block->instrs_ptr;
    while (instr != block->instrs_ptr_end) {
      bool replace_with_ssi = false;
      Object *obj = NULL; char *opt_info = NULL; int target_slot = 0;
      if (instr->type == INSTR_ACCESS_STRING_KEY) {
        AccessStringKeyInstr *aski = (AccessStringKeyInstr*) instr;
        if (object_known[aski->target_slot]) {
          replace_with_ssi = true;
          // Note: there's no need to gc-pin this, since it's
          // clearly a value that we can see anyways
          // (ie. it's covered via the gc link via context)
          obj = known_objects_table[aski->target_slot];
          opt_info = my_asprintf("inlined lookup to '%.*s'", aski->key_len, aski->key_ptr);
          target_slot = aski->target_slot;
        }
      }
      
      if (instr->type == INSTR_ALLOC_INT_OBJECT) {
        AllocIntObjectInstr *aioi = (AllocIntObjectInstr*) instr;
        replace_with_ssi = true;
        obj = alloc_int(state, aioi->value);
        gc_add_perm(state, obj);
        opt_info = my_asprintf("inlined alloc_int %i", aioi->value);
        target_slot = aioi->target_slot;
      }
      
      if (instr->type == INSTR_ALLOC_FLOAT_OBJECT) {
        AllocFloatObjectInstr *afoi = (AllocFloatObjectInstr*) instr;
        replace_with_ssi = true;
        obj = alloc_float(state, afoi->value);
        gc_add_perm(state, obj);
        opt_info = my_asprintf("inlined alloc_float %i", afoi->value);
        target_slot = afoi->target_slot;
      }
      
      if (instr->type == INSTR_ALLOC_STRING_OBJECT) {
        AllocStringObjectInstr *asoi = (AllocStringObjectInstr*) instr;
        replace_with_ssi = true;
        int len = strlen(asoi->value);
        obj = alloc_string(state, asoi->value, len);
        gc_add_perm(state, obj);
        opt_info = my_asprintf("inlined alloc_string %.*s", len, asoi->value);
        target_slot = asoi->target_slot;
      }
      
      if (replace_with_ssi) {
        SetSlotInstr *ssi = malloc(sizeof(SetSlotInstr));
        ssi->base.type = INSTR_SET_SLOT;
        ssi->target_slot = target_slot;
        ssi->value = obj;
        ssi->opt_info = opt_info;
        addinstr_like(builder, instr, sizeof(*ssi), (Instr*) ssi);
      } else {
        addinstr_like(builder, instr, instr_size(instr), instr);
      }
      
      instr = (Instr*) ((char*) instr + instr_size(instr));
    }
  }
  UserFunction *fn = build_function(builder);
  copy_fn_stats(uf, fn);
  return fn;
}

UserFunction *optimize(UserFunction *uf) {
  
  bool *primitive_slots;
  slot_is_primitive(uf, &primitive_slots);
  uf = inline_primitive_accesses(uf, primitive_slots);
  
  uf = redirect_predictable_lookup_misses(uf);
  
  /*if (uf->name) {
    fprintf(stderr, "static optimized %s to\n", uf->name);
    dump_fn(uf);
  }*/
  
  return uf;
}

UserFunction *optimize_runtime(VMState *state, UserFunction *uf, Object *context) {
  /*
  printf("runtime optimize %s with %p\n", uf->name, (void*) context);
  if (uf->name && strcmp(uf->name, "gear") == 0) print_recursive(state, context, false);
  printf("\n-----\n");
  */
  
  // moved here because it can be kind of expensive due to lazy coding, and I'm too lazy to fix it
  uf = access_vars_via_refslots(uf);
  uf = inline_static_lookups_to_constants(state, uf, context);
  {
    CFG cfg;
    cfg_build(&cfg, uf);
    
    RPost2Node rpost2node = cfg_get_reverse_postorder(&cfg);
    Node2RPost node2rpost = cfg_invert_rpost(&cfg, rpost2node);
    if (cfg.nodes_len > 1) {
      fprintf(stderr, "cfg reverse postorder: ");
      for (int i = 0; i < cfg.nodes_len; ++i) {
        fprintf(stderr, "%i ", node2rpost.ptr[i]);
      }
      fprintf(stderr, "\n");
    }
    int *sfidoms_ptr;
    cfg_build_sfidom_list(&cfg, rpost2node, node2rpost, &sfidoms_ptr);
    free(rpost2node.ptr);
    free(node2rpost.ptr);
    
    if (cfg.nodes_len > 1) {
      fprintf(stderr, "cfg dominance field: ");
      for (int i = 0; i < cfg.nodes_len; ++i) {
        fprintf(stderr, "%i ", sfidoms_ptr[i]);
      }
      fprintf(stderr, "\n");
    }
    free(sfidoms_ptr);
    if (cfg.nodes_len > 2) {
      cfg_dump("cfg.dot", &cfg);
      // abort();
    }
    cfg_destroy(&cfg);
  }
  
  if (uf->name) {
    fprintf(stderr, "runtime optimized %s to\n", uf->name);
    dump_fn(uf);
  }
  
  return uf;
}
