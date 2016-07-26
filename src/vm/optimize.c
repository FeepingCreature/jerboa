#include "vm/optimize.h"

#include <stdio.h>

#include "vm/builder.h"
#include "vm/cfg.h"
#include "gc.h"

// mark slots whose value is only
// used as parameter to other instructions and does not escape
// such as string keys
void slot_is_primitive(UserFunction *uf, bool** slots_p) {
  *slots_p = malloc(sizeof(bool) * uf->slots);
  bool *slots = *slots_p;
  for (int i = 0; i < uf->slots; ++i) slots[i] = true; // default assumption
  
  for (int i = 0; i < uf->body.blocks_len; ++i) {
    Instr *instr_cur = BLOCK_START(uf, i), *instr_end = BLOCK_END(uf, i);
    while (instr_cur != instr_end) {
#define CASE(KEY, TY) } break; \
        case KEY: { TY *instr = (TY*) instr_cur; (void) instr;
      switch (instr_cur->type) {
        case INSTR_INVALID: { abort();
          CASE(INSTR_GET_ROOT, GetRootInstr)
          CASE(INSTR_ALLOC_OBJECT, AllocObjectInstr)
            slots[instr->parent_slot] = false;
          CASE(INSTR_ALLOC_INT_OBJECT, AllocIntObjectInstr)
          CASE(INSTR_ALLOC_BOOL_OBJECT, AllocBoolObjectInstr)
          CASE(INSTR_ALLOC_FLOAT_OBJECT, AllocFloatObjectInstr)
          CASE(INSTR_ALLOC_ARRAY_OBJECT, AllocArrayObjectInstr)
          CASE(INSTR_ALLOC_STRING_OBJECT, AllocStringObjectInstr)
          CASE(INSTR_ALLOC_CLOSURE_OBJECT, AllocClosureObjectInstr)
          CASE(INSTR_CLOSE_OBJECT, CloseObjectInstr)
          CASE(INSTR_FREEZE_OBJECT, FreezeObjectInstr)
          CASE(INSTR_ACCESS, AccessInstr)
            if (instr->obj.kind == ARG_SLOT) slots[instr->obj.slot] = false;
          CASE(INSTR_ASSIGN, AssignInstr)
            if (instr->obj.kind == ARG_SLOT) slots[instr->obj.slot] = false;
            if (instr->value.kind == ARG_SLOT) slots[instr->value.slot] = false;
          // TODO inline key?
          CASE(INSTR_KEY_IN_OBJ, KeyInObjInstr)
            if (instr->key.kind == ARG_SLOT) slots[instr->key.slot] = false;
            if (instr->obj.kind == ARG_SLOT) slots[instr->obj.slot] = false;
          CASE(INSTR_INSTANCEOF, InstanceofInstr)
            if (instr->obj.kind == ARG_SLOT) slots[instr->obj.slot] = false;
            if (instr->proto.kind == ARG_SLOT) slots[instr->proto.slot] = false;
          CASE(INSTR_SET_CONSTRAINT, SetConstraintInstr)
            if (instr->obj.kind == ARG_SLOT) slots[instr->obj.slot] = false;
            if (instr->constraint.kind == ARG_SLOT) slots[instr->constraint.slot] = false;
          CASE(INSTR_CALL, CallInstr)
            if (instr->info.fn.kind == ARG_SLOT) slots[instr->info.fn.slot] = false;
            if (instr->info.this_arg.kind == ARG_SLOT) slots[instr->info.this_arg.slot] = false;
            for (int k = 0; k < instr->info.args_len; ++k) {
              Arg arg = ((Arg*)(&instr->info + 1))[k];
              if (arg.kind == ARG_SLOT) slots[arg.slot] = false;
            }
          CASE(INSTR_RETURN, ReturnInstr)
            if (instr->ret.kind == ARG_SLOT) slots[instr->ret.slot] = false;
          CASE(INSTR_BR, BranchInstr)
          CASE(INSTR_TESTBR, TestBranchInstr)
            if (instr->test.kind == ARG_SLOT) slots[instr->test.slot] = false;
          CASE(INSTR_PHI, PhiInstr)
            if (instr->arg1.kind == ARG_SLOT) slots[instr->arg1.slot] = false;
            if (instr->arg2.kind == ARG_SLOT) slots[instr->arg2.slot] = false;
            if (instr->target.kind == ARG_SLOT) slots[instr->target.slot] = false;
          CASE(INSTR_LAST, Instr) abort();
        } break;
        default: assert("Unhandled Instruction Type!" && false);
      }
#undef CASE
      instr_cur = (Instr*) ((char*) instr_cur + instr_size(instr_cur));
    }
  }
}

static Value *find_constant_slots(UserFunction *uf) {
  Value *slots = calloc(sizeof(Value), uf->slots);
  for (int i = 0; i < uf->body.blocks_len; ++i) {
    Instr *instr = BLOCK_START(uf, i), *instr_end = BLOCK_END(uf, i);
    while (instr != instr_end) {
      if (instr->type == INSTR_MOVE) {
        MoveInstr *mi = (MoveInstr*) instr;
        if (mi->source.kind == ARG_VALUE && mi->target.kind == ARG_SLOT) {
          slots[mi->target.slot] = mi->source.value;
        }
      }
      instr = (Instr*)((char*) instr + instr_size(instr));
    }
  }
  return slots;
}

static int *find_refslots(UserFunction *uf) {
  int *slots = calloc(sizeof(Value), uf->slots);
  for (int i = 0; i < uf->slots; ++i) slots[i] = -1;
  for (int i = 0; i < uf->body.blocks_len; ++i) {
    Instr *instr = BLOCK_START(uf, i), *instr_end = BLOCK_END(uf, i);
    while (instr != instr_end) {
      if (instr->type == INSTR_MOVE) {
        MoveInstr *mi = (MoveInstr*) instr;
        if (mi->source.kind == ARG_REFSLOT && mi->target.kind == ARG_SLOT) {
          slots[mi->target.slot] = mi->source.refslot;
        }
      }
      instr = (Instr*)((char*) instr + instr_size(instr));
    }
  }
  return slots;
}

typedef struct {
  int constraint_len;
  Object **constraint_ptr;
  Instr **constraint_imposed_here_ptr;
} ConstraintInfo;

typedef struct {
  bool static_object;
  int parent_slot;
  
  int fields_len;
  char **names_ptr;
  ConstraintInfo *constraints_ptr;
  
  FileRange *belongs_to;
  int context_slot;
  Instr *after_object_decl;
} SlotIsStaticObjInfo;

int static_info_find_field(SlotIsStaticObjInfo *rec, int name_len, char *name_ptr) {
  assert(rec->static_object);
  for (int k = 0; k < rec->fields_len; ++k) {
    int field_len = strlen(rec->names_ptr[k]);
    if (field_len == name_len && strncmp(rec->names_ptr[k], name_ptr, name_len) == 0) {
      return k;
    }
  }
  return -1;
}

#include "vm/dump.h"

// static object: allocated, assigned a few keys, and closed.
static void slot_is_static_object(UserFunction *uf, SlotIsStaticObjInfo **slots_p) {
  *slots_p = calloc(sizeof(SlotIsStaticObjInfo), uf->slots);
  
  CFG cfg;
  cfg_build(&cfg, uf);
  
  Value *constant_slots = find_constant_slots(uf);
  int *field_for_refslot = calloc(sizeof(int), uf->refslots);
  int *objslot_for_refslot = calloc(sizeof(int), uf->refslots);
  for (int i = 0; i < uf->refslots; ++i) {
    field_for_refslot[i] = -1;
    objslot_for_refslot[i] = -1;
  }
  
  for (int i = 0; i < uf->body.blocks_len; ++i) {
    Instr *instr = BLOCK_START(uf, i), *instr_end = BLOCK_END(uf, i);
    while (instr != instr_end) {
      if (instr->type == INSTR_DEFINE_REFSLOT) {
        DefineRefslotInstr *dri = (DefineRefslotInstr*) instr;
        instr = (Instr*) (dri + 1);
        int obj_slot = dri->obj_slot;
        SlotIsStaticObjInfo *rec = &((*slots_p)[obj_slot]);
        if (rec->static_object) {
          int field = static_info_find_field(rec, dri->key_len, dri->key_ptr);
          if (field != -1) {
            objslot_for_refslot[dri->target_refslot] = obj_slot;
            field_for_refslot[dri->target_refslot] = field;
          }
        }
        continue;
      }
      if (instr->type == INSTR_ALLOC_OBJECT) {
        AllocObjectInstr *alobi = (AllocObjectInstr*) instr;
        instr = (Instr*) (alobi + 1);
        bool failed = false;
        char **names_ptr = 0; int fields_len = 0;
        while (instr != instr_end) {
          if (instr->type == INSTR_ASSIGN_STRING_KEY) {
            AssignStringKeyInstr *aski = (AssignStringKeyInstr*) instr;
            if (aski->type != ASSIGN_PLAIN) { failed = true; break; }
            if (aski->obj.kind != ARG_SLOT) { failed = true; break; }
            if (aski->obj.slot != alobi->target_slot) { failed = true; break; }
            
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
        (*slots_p)[target_slot].constraints_ptr = calloc(sizeof(ConstraintInfo), fields_len);
        (*slots_p)[target_slot].belongs_to = instr->belongs_to;
        (*slots_p)[target_slot].context_slot = instr->context_slot;
        
        instr = (Instr*)((CloseObjectInstr*) instr + 1);
        (*slots_p)[target_slot].after_object_decl = instr;
        continue;
      }
      instr = (Instr*)((char*) instr + instr_size(instr));
    }
  }
  
  // gather all constraints
  for (int i = 0; i < uf->body.blocks_len; ++i) {
    Instr *instr = BLOCK_START(uf, i), *instr_end = BLOCK_END(uf, i);
    while (instr != instr_end) {
      if (instr->type == INSTR_SET_CONSTRAINT_STRING_KEY) {
        SetConstraintStringKeyInstr *scski = (SetConstraintStringKeyInstr*) instr;
        if ((scski->constraint.kind == ARG_VALUE || scski->constraint.kind == ARG_SLOT)
          && scski->obj.kind == ARG_SLOT)
        {
          Object *constraint;
          if (scski->constraint.kind == ARG_VALUE) constraint = OBJ_OR_NULL(scski->constraint.value);
          else constraint = OBJ_OR_NULL(constant_slots[scski->constraint.slot]);
          SlotIsStaticObjInfo *rec = &(*slots_p)[scski->obj.slot];
          
          if (constraint && rec->static_object) {
            int field = static_info_find_field(rec, scski->key_len, scski->key_ptr);
            assert(field != -1); // wat, object was closed and we set a constraint on a field that doesn't exist??
            ConstraintInfo *info = &rec->constraints_ptr[field];
            info->constraint_ptr = realloc(info->constraint_ptr, sizeof(Object*) * ++info->constraint_len);
            info->constraint_ptr[info->constraint_len - 1] = constraint;
            info->constraint_imposed_here_ptr = realloc(info->constraint_imposed_here_ptr, sizeof(Instr*) * info->constraint_len);
            info->constraint_imposed_here_ptr[info->constraint_len - 1] = instr;
          }
          instr = (Instr*) (scski + 1);
          continue;
        }
      }
      
      // 3 instance of: %27 = &0 instanceof <obj: 0x1985e90>
      // 3 test-branch: %27 ? <7> : <9>
      // and <7> is only targeted by one block
      // thus, impose a type constraint on &0 in <7>
      if (instr->type == INSTR_INSTANCEOF) {
        InstanceofInstr *ins = (InstanceofInstr*) instr;
        instr = (Instr*) (ins + 1);
        if (instr->type == INSTR_TESTBR) {
          TestBranchInstr *tbr = (TestBranchInstr*) instr;
          instr = (Instr*) (tbr + 1);
          if (ins->target.kind == ARG_SLOT && tbr->test.kind == ARG_SLOT
            && ins->target.slot == tbr->test.slot
            && ins->proto.kind == ARG_VALUE && IS_OBJ(ins->proto.value)
            && ins->obj.kind == ARG_REFSLOT
            && cfg.nodes_ptr[tbr->true_blk].pred_len == 1)
          {
            assert(cfg.nodes_ptr[tbr->true_blk].pred_ptr[0] == i);
            Object *constraint = AS_OBJ(ins->proto.value);
            int refslot = ins->obj.refslot;
            if (objslot_for_refslot[refslot] != -1) {
              SlotIsStaticObjInfo *rec = &(*slots_p)[objslot_for_refslot[refslot]];
              int field = field_for_refslot[refslot];
              assert(field != -1);
              if (rec->static_object) {
                Instr *trueblk_start = BLOCK_START(uf, tbr->true_blk);
                ConstraintInfo *info = &rec->constraints_ptr[field];
                info->constraint_ptr = realloc(info->constraint_ptr, sizeof(Object*) * ++info->constraint_len);
                info->constraint_ptr[info->constraint_len - 1] = constraint;
                info->constraint_imposed_here_ptr = realloc(info->constraint_imposed_here_ptr, sizeof(Instr*) * info->constraint_len);
                info->constraint_imposed_here_ptr[info->constraint_len - 1] = trueblk_start;
              }
            }
          }
        }
        continue;
      }
      
      instr = (Instr*)((char*) instr + instr_size(instr));
    }
  }
  
  cfg_destroy(&cfg);
  
  free(constant_slots);
  free(objslot_for_refslot);
  free(field_for_refslot);
}

static void copy_fn_stats(UserFunction *from, UserFunction *to) {
  to->slots = from->slots;
  to->refslots = from->refslots;
  to->arity = from->arity;
  to->name = from->name;
  to->is_method = from->is_method;
  to->variadic_tail = from->variadic_tail;
}

UserFunction *redirect_predictable_lookup_misses(UserFunction *uf) {
  SlotIsStaticObjInfo *info;
  slot_is_static_object(uf, &info);
  
  FunctionBuilder builder = {0};
  builder.block_terminated = true;
  
  for (int i = 0; i < uf->body.blocks_len; ++i) {
    new_block(&builder);
    
    Instr *instr = BLOCK_START(uf, i), *instr_end = BLOCK_END(uf, i);
    while (instr != instr_end) {
      if (instr->type == INSTR_ACCESS_STRING_KEY) {
        AccessStringKeyInstr *aski = (AccessStringKeyInstr*) instr;
        AccessStringKeyInstr aski_new = *aski;
        while (true) {
          if (aski_new.obj.kind != ARG_SLOT) break;
          int obj_slot = aski_new.obj.slot;
          if (!info[obj_slot].static_object) break;
          int field = static_info_find_field(&info[obj_slot], aski->key_len, aski->key_ptr);
          if (field != -1) break;
          // since the key was not found in obj
          // we can know statically that the lookup will
          // not succeed at runtime either
          // (since the object is closed, its keys are statically known)
          // so instead look up in the (known) parent object from the start
          aski_new.obj = (Arg) { .kind = ARG_SLOT, .slot = info[obj_slot].parent_slot };
        }
        addinstr_like(&builder, instr, sizeof(aski_new), (Instr*) &aski_new);
        instr = (Instr*) (aski + 1);
        continue;
      }
      
      if (instr->type == INSTR_ASSIGN_STRING_KEY) {
        AssignStringKeyInstr *aski = (AssignStringKeyInstr*) instr;
        if (aski->type == ASSIGN_EXISTING) {
          AssignStringKeyInstr aski_new = *aski;
          if (aski_new.obj.kind == ARG_SLOT) {
            while (true) {
              int obj_slot = aski_new.obj.slot;
              if (!info[obj_slot].static_object) break;
              int field = static_info_find_field(&info[obj_slot], strlen(aski->key), aski->key);
              if (field != -1) break; // key was found, we're at the right object
              aski_new.obj.slot = info[obj_slot].parent_slot;
            }
            addinstr_like(&builder, instr, sizeof(aski_new), (Instr*) &aski_new);
            instr = (Instr*) (aski + 1);
            continue;
          }
        }
      }
      
      addinstr_like(&builder, instr, instr_size(instr), instr);
      instr = (Instr*) ((char*) instr + instr_size(instr));
    }
  }
  UserFunction *fn = build_function(&builder);
  copy_fn_stats(uf, fn);
  free_function(uf);
  free(info);
  return fn;
}

UserFunction *access_vars_via_refslots(UserFunction *uf) {
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
  
  FunctionBuilder builder = {0};
  builder.refslot_base = uf->refslots;
  builder.block_terminated = true;
  
  // since the object accesses must dominate the object declaration,
  // the refslot accesses will also dominate the refslot declaration.
  
  for (int i = 0; i < uf->body.blocks_len; ++i) {
    new_block(&builder);
    
    Instr *instr = BLOCK_START(uf, i), *instr_end = BLOCK_END(uf, i);
    while (instr != instr_end) {
      // O(n²) but nbd
      // TODO bdaa
      // TODO store block/instr index with after_object_decl, invert into array of arrays for instrs
      for (int k = 0; k < info_slots_len; ++k) {
        int slot = info_slots_ptr[k];
        if (instr == info[slot].after_object_decl) {
          use_range_start(&builder, info[slot].belongs_to);
          builder.scope = info[slot].context_slot;
          for (int l = 0; l < info[slot].fields_len; ++l) {
            ref_slots_ptr[slot][l] = addinstr_def_refslot(&builder, slot, info[slot].names_ptr[l]);
          }
          use_range_end(&builder, info[slot].belongs_to);
          obj_refslots_initialized[slot] = true;
        }
      }
      
      if (instr->type == INSTR_ACCESS_STRING_KEY) {
        AccessStringKeyInstr *aski = (AccessStringKeyInstr*) instr;
        if (aski->obj.kind == ARG_SLOT && aski->target.kind == ARG_SLOT) {
          int obj_slot = aski->obj.slot;
          char *key = aski->key_ptr;
          if (info[obj_slot].static_object && obj_refslots_initialized[obj_slot]) {
            bool continue_outer = false;
            for (int k = 0; k < info[obj_slot].fields_len; ++k) {
              char *name = info[obj_slot].names_ptr[k];
              if (strcmp(key, name) == 0) {
                int refslot = ref_slots_ptr[obj_slot][k];
                use_range_start(&builder, instr->belongs_to);
                builder.scope = instr->context_slot;
                addinstr_move(&builder,
                              (Arg){.kind=ARG_REFSLOT,.refslot=refslot},
                              aski->target);
                use_range_end(&builder, instr->belongs_to);
                instr = (Instr*) (aski + 1);
                continue_outer = true;
                break;
              }
            }
            if (continue_outer) continue;
          }
        }
      }
      
      if (instr->type == INSTR_ASSIGN_STRING_KEY) {
        AssignStringKeyInstr *aski = (AssignStringKeyInstr*) instr;
        if (aski->obj.kind == ARG_SLOT) {
          int obj_slot = aski->obj.slot;
          char *key = aski->key;
          if (info[obj_slot].static_object && obj_refslots_initialized[obj_slot]) {
            bool continue_outer = false;
            for (int k = 0; k < info[obj_slot].fields_len; ++k) {
              char *name = info[obj_slot].names_ptr[k];
              if (strcmp(key, name) == 0) {
                int refslot = ref_slots_ptr[obj_slot][k];
                use_range_start(&builder, instr->belongs_to);
                builder.scope = instr->context_slot;
                addinstr_move(&builder, aski->value, (WriteArg){.kind=ARG_REFSLOT,.refslot=refslot});
                use_range_end(&builder, instr->belongs_to);
                instr = (Instr*) (aski + 1);
                continue_outer = true;
                break;
              }
            }
            if (continue_outer) continue;
          }
        }
      }
      
      addinstr_like(&builder, instr, instr_size(instr), instr);
      instr = (Instr*) ((char*) instr + instr_size(instr));
    }
  }
  UserFunction *fn = build_function(&builder);
  int new_refslots = fn->refslots;
  copy_fn_stats(uf, fn);
  free_function(uf);
  fn->refslots = new_refslots; // do update this
  free(info);
  free(info_slots_ptr);
  free(obj_refslots_initialized);
  for (int i = 0; i < fn->slots; ++i) {
    free(ref_slots_ptr[i]);
  }
  free(ref_slots_ptr);
  
  return fn;
}

UserFunction *inline_primitive_accesses(UserFunction *uf, bool *prim_slot) {
  FunctionBuilder builder = {0};
  builder.block_terminated = true;
  
  char **slot_table_ptr = NULL;
  int slot_table_len = 0;
  
  for (int i = 0; i < uf->body.blocks_len; ++i) {
    new_block(&builder);
    
    Instr *instr = BLOCK_START(uf, i), *instr_end = BLOCK_END(uf, i);
    while (instr != instr_end) {
      AllocStringObjectInstr *asoi = (AllocStringObjectInstr*) instr;
      AccessInstr *acci = (AccessInstr*) instr;
      AssignInstr *assi = (AssignInstr*) instr;
      SetConstraintInstr *sci = (SetConstraintInstr*) instr;
      if (instr->type == INSTR_ALLOC_STRING_OBJECT
        && asoi->target.kind == ARG_SLOT
        && prim_slot[asoi->target.slot] == true)
      {
        if (slot_table_len < asoi->target.slot + 1) {
          slot_table_ptr = realloc(slot_table_ptr, sizeof(char*) * (asoi->target.slot + 1));
          for (int i = slot_table_len; i < asoi->target.slot + 1; ++i) {
            slot_table_ptr[i] = NULL;
          }
          slot_table_len = asoi->target.slot + 1;
        }
        slot_table_ptr[asoi->target.slot] = asoi->value;
        instr = (Instr*)(asoi + 1);
        continue; // no need to add, we're fully inlining this
      }
      if (instr->type == INSTR_ACCESS
        && acci->key.kind == ARG_SLOT
        && acci->key.slot < slot_table_len && slot_table_ptr[acci->key.slot] != NULL)
      {
        char *key_ptr = slot_table_ptr[acci->key.slot];
        int key_len = strlen(key_ptr);
        AccessStringKeyInstr aski = {
          .base = { .type = INSTR_ACCESS_STRING_KEY },
          .obj = acci->obj,
          .key_slot = acci->key.slot,
          .target = acci->target,
          .key_ptr = key_ptr,
          .key_len = key_len,
          .key_hash = hash(key_ptr, key_len)
        };
        addinstr_like(&builder, instr, sizeof(aski), (Instr*) &aski);
        instr = (Instr*)(acci + 1);
        continue;
      }
      if (instr->type == INSTR_ASSIGN
        && assi->key.kind == ARG_SLOT
        && assi->key.slot < slot_table_len && slot_table_ptr[assi->key.slot] != NULL)
      {
        AssignStringKeyInstr aski = {
          .base = { .type = INSTR_ASSIGN_STRING_KEY },
          .obj = assi->obj,
          .value = assi->value,
          .key = slot_table_ptr[assi->key.slot],
          .type = assi->type
        };
        addinstr_like(&builder, instr, sizeof(aski), (Instr*) &aski);
        instr = (Instr*)(assi + 1);
        continue;
      }
      if (instr->type == INSTR_SET_CONSTRAINT && sci->key.kind == ARG_SLOT
        && sci->key.slot < slot_table_len && slot_table_ptr[sci->key.slot] != NULL
      ) {
        char *key_ptr = slot_table_ptr[sci->key.slot];
        SetConstraintStringKeyInstr scski = {
          .base = { .type = INSTR_SET_CONSTRAINT_STRING_KEY },
          .obj = sci->obj,
          .constraint = sci->constraint,
          .key_ptr = key_ptr,
          .key_len = strlen(key_ptr)
        };
        addinstr_like(&builder, instr, sizeof(scski), (Instr*) &scski);
        instr = (Instr*)(sci + 1);
        continue;
      }
      addinstr_like(&builder, instr, instr_size(instr), instr);
      
      instr = (Instr*) ((char*) instr + instr_size(instr));
    }
  }
  UserFunction *fn = build_function(&builder);
  copy_fn_stats(uf, fn);
  free_function(uf);
  return fn;
}

#include "print.h"

// if lookup for "key" in "context" will always return the same value, return it.
Value lookup_statically(Object *obj, char *key_ptr, int key_len, size_t hashv, bool *key_found_p) {
  *key_found_p = false;
  while (obj) {
    TableEntry *entry = table_lookup_with_hash(&obj->tbl, key_ptr, key_len, hashv);
    if (entry) {
      // hit, but the value might change later! bad!
      if (!(obj->flags & OBJ_FROZEN)) {
        // printf("hit for %.*s, but object wasn't frozen\n", key_len, key_ptr);
        return VNULL;
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
      return VNULL;
    }
    obj = obj->parent;
  }
  return VNULL; // no hits.
}

UserFunction *remove_dead_slot_writes(UserFunction *uf) {
  bool *slot_live = calloc(sizeof(bool), uf->slots);
  for (int i = 0; i < uf->body.blocks_len; ++i) {
    Instr *instr_cur = BLOCK_START(uf, i), *instr_end = BLOCK_END(uf, i);
    while (instr_cur != instr_end) {
#define CASE(KEY, TY) instr_cur = (Instr*) ((char*) instr_cur + instr_size(instr_cur)); } break; \
        case KEY: { TY *instr = (TY*) instr_cur; (void) instr;
      switch (instr_cur->type) {
        // mark all slots that are accessed in an instr other than INSTR_WRITE_SLOT
        case INSTR_INVALID: { abort();
          CASE(INSTR_GET_ROOT, GetRootInstr)
            slot_live[instr->slot] = true;
          CASE(INSTR_ALLOC_OBJECT, AllocObjectInstr)
            slot_live[instr->parent_slot] = true;
          CASE(INSTR_ALLOC_INT_OBJECT, AllocIntObjectInstr)
            if (instr->target.kind == ARG_SLOT) slot_live[instr->target.slot] = true;
          CASE(INSTR_ALLOC_BOOL_OBJECT, AllocBoolObjectInstr)
            if (instr->target.kind == ARG_SLOT) slot_live[instr->target.slot] = true;
          CASE(INSTR_ALLOC_FLOAT_OBJECT, AllocFloatObjectInstr)
            if (instr->target.kind == ARG_SLOT) slot_live[instr->target.slot] = true;
          CASE(INSTR_ALLOC_ARRAY_OBJECT, AllocArrayObjectInstr)
            if (instr->target.kind == ARG_SLOT) slot_live[instr->target.slot] = true;
          CASE(INSTR_ALLOC_STRING_OBJECT, AllocStringObjectInstr)
            if (instr->target.kind == ARG_SLOT) slot_live[instr->target.slot] = true;
          CASE(INSTR_ALLOC_CLOSURE_OBJECT, AllocClosureObjectInstr)
            if (instr->target.kind == ARG_SLOT) slot_live[instr->target.slot] = true;
            slot_live[instr->base.context_slot] = true;
          CASE(INSTR_CLOSE_OBJECT, CloseObjectInstr)
            slot_live[instr->slot] = true;
          CASE(INSTR_FREEZE_OBJECT, FreezeObjectInstr)
            slot_live[instr->slot] = true;
          CASE(INSTR_ACCESS, AccessInstr)
            if (instr->obj.kind == ARG_SLOT) slot_live[instr->obj.slot] = true;
            if (instr->key.kind == ARG_SLOT) slot_live[instr->key.slot] = true;
            if (instr->target.kind == ARG_SLOT) slot_live[instr->target.slot] = true;
          CASE(INSTR_ASSIGN, AssignInstr)
            if (instr->obj.kind == ARG_SLOT) slot_live[instr->obj.slot] = true;
            if (instr->key.kind == ARG_SLOT) slot_live[instr->key.slot] = true;
            if (instr->value.kind == ARG_SLOT) slot_live[instr->value.slot] = true;
          // TODO inline key?
          CASE(INSTR_KEY_IN_OBJ, KeyInObjInstr)
            if (instr->key.kind == ARG_SLOT) slot_live[instr->key.slot] = true;
            if (instr->obj.kind == ARG_SLOT) slot_live[instr->obj.slot] = true;
            if (instr->target.kind == ARG_SLOT) slot_live[instr->target.slot] = true;
          CASE(INSTR_INSTANCEOF, InstanceofInstr)
            if (instr->obj.kind == ARG_SLOT) slot_live[instr->obj.slot] = true;
            if (instr->proto.kind == ARG_SLOT) slot_live[instr->proto.slot] = true;
          CASE(INSTR_SET_CONSTRAINT, SetConstraintInstr)
            if (instr->obj.kind == ARG_SLOT) slot_live[instr->obj.slot] = true;
            if (instr->key.kind == ARG_SLOT) slot_live[instr->key.slot] = true;
            if (instr->constraint.kind == ARG_SLOT) slot_live[instr->constraint.slot] = true;
          CASE(INSTR_CALL, CallInstr)
            if (instr->info.fn.kind == ARG_SLOT) slot_live[instr->info.fn.slot] = true;
            if (instr->info.this_arg.kind == ARG_SLOT) slot_live[instr->info.this_arg.slot] = true;
            for (int k = 0; k < instr->info.args_len; ++k) {
              Arg arg = ((Arg*)(&instr->info + 1))[k];
              if (arg.kind == ARG_SLOT) slot_live[arg.slot] = true;
            }
          CASE(INSTR_RETURN, ReturnInstr)
            if (instr->ret.kind == ARG_SLOT) slot_live[instr->ret.slot] = true;
          CASE(INSTR_BR, BranchInstr)
          CASE(INSTR_TESTBR, TestBranchInstr)
            if (instr->test.kind == ARG_SLOT) slot_live[instr->test.slot] = true;
          CASE(INSTR_PHI, PhiInstr)
            if (instr->arg1.kind == ARG_SLOT) slot_live[instr->arg1.slot] = true;
            if (instr->arg2.kind == ARG_SLOT) slot_live[instr->arg2.slot] = true;
          CASE(INSTR_ACCESS_STRING_KEY, AccessStringKeyInstr)
            if (instr->obj.kind == ARG_SLOT) slot_live[instr->obj.slot] = true;
            if (instr->target.kind == ARG_SLOT) slot_live[instr->target.slot] = true;
          CASE(INSTR_ASSIGN_STRING_KEY, AssignStringKeyInstr)
            if (instr->obj.kind == ARG_SLOT) slot_live[instr->obj.slot] = true;
            if (instr->value.kind == ARG_SLOT) slot_live[instr->value.slot] = true;
          CASE(INSTR_SET_CONSTRAINT_STRING_KEY, SetConstraintStringKeyInstr)
            if (instr->obj.kind == ARG_SLOT) slot_live[instr->obj.slot] = true;
            if (instr->constraint.kind == ARG_SLOT) slot_live[instr->constraint.slot] = true;
          CASE(INSTR_MOVE, MoveInstr)
            if (instr->source.kind == ARG_SLOT) slot_live[instr->source.slot] = true;
          CASE(INSTR_DEFINE_REFSLOT, DefineRefslotInstr)
            slot_live[instr->obj_slot] = true;
          CASE(INSTR_ALLOC_STATIC_OBJECT, AllocStaticObjectInstr)
            for (int k = 0; k < instr->info_len; ++k)
              slot_live[ASOI_INFO(instr)[k].slot] = true;
          CASE(INSTR_LAST, Instr) abort();
        } break;
        default: assert("Unhandled Instruction Type!" && false);
      }
#undef CASE
    }
  }
  
  FunctionBuilder builder = {0};
  builder.block_terminated = true;
  
  for (int i = 0; i < uf->body.blocks_len; ++i) {
    new_block(&builder);
    
    Instr *instr = BLOCK_START(uf, i), *instr_end = BLOCK_END(uf, i);
    while (instr != instr_end) {
      bool add = true;
      if (instr->type == INSTR_MOVE) {
        MoveInstr *mi = (MoveInstr*) instr;
        if (mi->target.kind == ARG_SLOT && !slot_live[mi->target.slot]) {
          add = false;
        }
      }
      if (add) addinstr_like(&builder, instr, instr_size(instr), instr);
      
      instr = (Instr*) ((char*) instr + instr_size(instr));
    }
  }
  UserFunction *fn = build_function(&builder);
  copy_fn_stats(uf, fn);
  free_function(uf);
  return fn;
}

UserFunction *call_functions_directly(VMState *state, UserFunction *uf) {
FunctionBuilder builder = {0};
  builder.block_terminated = true;
  
  for (int i = 0; i < uf->body.blocks_len; ++i) {
    new_block(&builder);
    
    Instr *instr_cur = BLOCK_START(uf, i), *instr_end = BLOCK_END(uf, i);
    while (instr_cur != instr_end) {
      if (instr_cur->type == INSTR_CALL) {
        CallInstr *instr = (CallInstr*) instr_cur;
        if (instr->info.fn.kind == ARG_VALUE && IS_OBJ(instr->info.fn.value)) {
          Object *fn_obj_n = AS_OBJ(instr->info.fn.value);
          if (fn_obj_n->parent == state->shared->vcache.function_base) {
            int size = sizeof(CallFunctionDirectInstr) + sizeof(Arg) * instr->size;
            CallFunctionDirectInstr *cfdi = alloca(size);
            cfdi->base = (Instr) {
              .type = INSTR_CALL_FUNCTION_DIRECT,
              .belongs_to = instr->base.belongs_to
            };
            cfdi->size = size;
            cfdi->fn = ((FunctionObject*)fn_obj_n)->fn_ptr;
            cfdi->info = instr->info;
            for (int i = 0; i < instr->info.args_len; ++i) {
              ((Arg*)(&cfdi->info + 1))[i] = ((Arg*)(&instr->info + 1))[i];
            }
            addinstr_like(&builder, instr_cur, size, (Instr*) cfdi);
            instr_cur = (Instr*) ((char*) instr + instr->size);
            continue;
          }
        }
      }
      
      addinstr_like(&builder, instr_cur, instr_size(instr_cur), instr_cur);
      instr_cur = (Instr*) ((char*) instr_cur + instr_size(instr_cur));
    }
  }
  UserFunction *fn = build_function(&builder);
  copy_fn_stats(uf, fn);
  free_function(uf);
  return fn;
}

bool dominates(UserFunction *uf, Node2RPost node2rpost, int *sfidoms_ptr, Instr *earlier, Instr *later);

UserFunction *inline_static_lookups_to_constants(VMState *state, UserFunction *uf, Object *context, bool free_fn_after) {
  SlotIsStaticObjInfo *static_info;
  slot_is_static_object(uf, &static_info);
  CFG cfg;
  cfg_build(&cfg, uf);
  
  RPost2Node rpost2node = cfg_get_reverse_postorder(&cfg);
  Node2RPost node2rpost = cfg_invert_rpost(&cfg, rpost2node);
  int *sfidoms_ptr = cfg_build_sfidom_list(&cfg, rpost2node, node2rpost);
  
  Object *int_base = state->shared->vcache.int_base;
  Object *float_base = state->shared->vcache.float_base;
  
  bool *object_known = calloc(sizeof(bool), uf->slots);
  Value *known_values_table = calloc(sizeof(Value), uf->slots);
  object_known[1] = true;
  known_values_table[1] = OBJ2VAL(context);
  
  ConstraintInfo **slot_constraints = calloc(sizeof(ConstraintInfo*), uf->slots);
  ConstraintInfo **refslot_constraints = calloc(sizeof(ConstraintInfo*), uf->slots);
  
  // prepass: gather constraints
  for (int i = 0; i < uf->body.blocks_len; ++i) {
    Instr *instr = BLOCK_START(uf, i), *instr_end = BLOCK_END(uf, i);
    while (instr != instr_end) {
      if (instr->type == INSTR_ACCESS_STRING_KEY) {
        AccessStringKeyInstr *aski = (AccessStringKeyInstr*) instr;
        if (aski->obj.kind == ARG_SLOT && aski->target.kind == ARG_SLOT) {
          SlotIsStaticObjInfo *rec = &static_info[aski->obj.slot];
          if (rec->static_object) {
            int field = static_info_find_field(rec, aski->key_len, aski->key_ptr);
            assert(field != -1);
            slot_constraints[aski->target.slot] = &rec->constraints_ptr[field];
          }
        }
      }
      if (instr->type == INSTR_DEFINE_REFSLOT) {
        DefineRefslotInstr *dri = (DefineRefslotInstr*) instr;
        SlotIsStaticObjInfo *rec = &static_info[dri->obj_slot];
        if (rec->static_object) {
          int field = static_info_find_field(rec, dri->key_len, dri->key_ptr);
          if (field != -1) {
            refslot_constraints[dri->target_refslot] = &rec->constraints_ptr[field];
          }
        }
      }
      instr = (Instr*) ((char*) instr + instr_size(instr));
    }
  }
  
  for (int i = 0; i < uf->body.blocks_len; ++i) {
    Instr *instr = BLOCK_START(uf, i), *instr_end = BLOCK_END(uf, i);
    while (instr != instr_end) {
      if (instr->type == INSTR_MOVE) {
        MoveInstr *mi = (MoveInstr*) instr;
        if (mi->target.kind == ARG_SLOT && mi->source.kind == ARG_VALUE) {
          object_known[mi->target.slot] = true;
          known_values_table[mi->target.slot] = mi->source.value;
        }
      }
      
      AccessStringKeyInstr *aski = (AccessStringKeyInstr*) instr;
      if (instr->type == INSTR_ACCESS_STRING_KEY && aski->target.kind == ARG_SLOT) {
        if (aski->obj.kind == ARG_SLOT && aski->target.kind == ARG_SLOT) {
          if (object_known[aski->obj.slot]) {
            Value known_val = known_values_table[aski->obj.slot];
            bool key_found;
            Value static_lookup = lookup_statically(closest_obj(state, known_val),
                                                      aski->key_ptr, aski->key_len, aski->key_hash,
                                                      &key_found);
            if (key_found) {
              object_known[aski->target.slot] = true;
              known_values_table[aski->target.slot] = static_lookup;
            }
          }
        }
        
        ConstraintInfo *constraints = NULL;
        if (aski->obj.kind == ARG_SLOT && slot_constraints[aski->obj.slot]) {
          constraints = slot_constraints[aski->obj.slot];
        }
        if (aski->obj.kind == ARG_REFSLOT && refslot_constraints[aski->obj.refslot]) {
          constraints = refslot_constraints[aski->obj.refslot];
        }
        if (constraints) {
          int num_dominant = 0;
          // fprintf(stderr, "typed object go! %i constraints\n", constraints->constraint_len);
          for (int k = 0; k < constraints->constraint_len; ++k) {
            Object *constraint = constraints->constraint_ptr[k];
            Instr *location = constraints->constraint_imposed_here_ptr[k];
            if (constraint == int_base || constraint == float_base) { // primitives, always closed
              if (dominates(uf, node2rpost, sfidoms_ptr, location, instr)) {
                object_known[aski->target.slot] = true;
                bool key_found = false;
                known_values_table[aski->target.slot] = object_lookup_with_hash(constraint, aski->key_ptr, aski->key_len, aski->key_hash, &key_found);
                if (!key_found) {
                  fprintf(stderr, "wat? static lookup on primitive-constrained field to %.*s not found in constraint object??\n", aski->key_len, aski->key_ptr);
                  abort();
                }
                num_dominant ++;
              }
            }
            if (num_dominant > 1) {
              fprintf(stderr, "WARN multiple dominant constraints on the same field. TODO merge?");
            }
          }
        }
      }
      instr = (Instr*) ((char*) instr + instr_size(instr));
    }
  }
  cfg_destroy(&cfg);
  free(rpost2node.ptr);
  free(node2rpost.ptr);
  free(sfidoms_ptr);
  free(static_info);
  
  FunctionBuilder builder = {0};
  builder.block_terminated = true;
  
  for (int i = 0; i < uf->body.blocks_len; ++i) {
    new_block(&builder);
    
    Instr *instr = BLOCK_START(uf, i), *instr_end = BLOCK_END(uf, i);
    while (instr != instr_end) {
      bool replace_with_mv = false;
      Value val = VNULL; char *opt_info = NULL; WriteArg target;
      if (instr->type == INSTR_ACCESS_STRING_KEY) {
        AccessStringKeyInstr *aski = (AccessStringKeyInstr*) instr;
        if (aski->target.kind == ARG_SLOT && object_known[aski->target.slot]) {
          replace_with_mv = true;
          // Note: there's no need to gc-pin this, since it's
          // clearly a value that we can see anyways
          // (ie. it's covered via the gc link via context)
          val = known_values_table[aski->target.slot];
          opt_info = my_asprintf("inlined lookup to '%.*s'", aski->key_len, aski->key_ptr);
          target = aski->target;
        }
      }
      
      if (instr->type == INSTR_ALLOC_INT_OBJECT) {
        AllocIntObjectInstr *aioi = (AllocIntObjectInstr*) instr;
        replace_with_mv = true;
        val = INT2VAL(aioi->value);
        opt_info = my_asprintf("inlined alloc_int %i", aioi->value);
        target = aioi->target;
      }
      
      if (instr->type == INSTR_ALLOC_BOOL_OBJECT) {
        AllocBoolObjectInstr *aboi = (AllocBoolObjectInstr*) instr;
        replace_with_mv = true;
        val = BOOL2VAL(aboi->value);
        opt_info = my_asprintf("inlined alloc_bool %s", aboi->value?"true":"false");
        target = aboi->target;
      }
      
      if (instr->type == INSTR_ALLOC_FLOAT_OBJECT) {
        AllocFloatObjectInstr *afoi = (AllocFloatObjectInstr*) instr;
        replace_with_mv = true;
        val = FLOAT2VAL(afoi->value);
        opt_info = my_asprintf("inlined alloc_float %f", afoi->value);
        target = afoi->target;
      }
      
      if (instr->type == INSTR_ALLOC_STRING_OBJECT) {
        AllocStringObjectInstr *asoi = (AllocStringObjectInstr*) instr;
        replace_with_mv = true;
        Object *obj = AS_OBJ(make_string_static(state, asoi->value));
        obj->flags |= OBJ_IMMORTAL;
        val = OBJ2VAL(obj);
        opt_info = my_asprintf("inlined alloc_string %s", asoi->value);
        target = asoi->target;
      }
      
      if (replace_with_mv) {
        MoveInstr mi = {
          .base = { .type = INSTR_MOVE },
          .source = (Arg) { .kind = ARG_VALUE, .value = val },
          .target = target,
          .opt_info = opt_info
        };
        addinstr_like(&builder, instr, sizeof(mi), (Instr*) &mi);
      } else {
        addinstr_like(&builder, instr, instr_size(instr), instr);
      }
      
      instr = (Instr*) ((char*) instr + instr_size(instr));
    }
  }
  UserFunction *fn = build_function(&builder);
  copy_fn_stats(uf, fn);
  if (free_fn_after) free_function(uf);
  return fn;
}

UserFunction *optimize(UserFunction *uf) {
  
  bool *primitive_slots;
  slot_is_primitive(uf, &primitive_slots);
  uf = inline_primitive_accesses(uf, primitive_slots);
  
  uf = redirect_predictable_lookup_misses(uf);
  
  /*
  if (uf->name) {
    fprintf(stderr, "static optimized %s to\n", uf->name);
    dump_fn(NULL, uf);
  }
  */
  
  return uf;
}

// does earlier dominate later?
// note: if this is too slow, make a struct InstrLocation { int block; Instr *instr; }
bool dominates(UserFunction *uf, Node2RPost node2rpost, int *sfidoms_ptr, Instr *earlier, Instr *later) {
  int blk_earlier = -1, blk_later = -1;
  for (int i = 0; i < uf->body.blocks_len; ++i) {
    Instr *blk_start = BLOCK_START(uf, i), *blk_end = BLOCK_END(uf, i);
    if (earlier >= blk_start && earlier < blk_end) {
      blk_earlier = i;
    }
    if (later >= blk_start && later < blk_end) {
      blk_later = i;
    }
    if (blk_earlier != -1 && blk_later != -1) break;
  }
  if (blk_earlier == -1) {
    fprintf(stderr, "start instr not in function\n");
    abort();
  }
  if (blk_later == -1) {
    fprintf(stderr, "end instr not in function\n");
    abort();
  }
  if (blk_earlier == blk_later) {
    return earlier <= later;
  }
  // is blk_earlier in blk_later's dominance set?
  
  if (node2rpost.ptr[blk_earlier] == -1 || node2rpost.ptr[blk_later] == -1) {
    return false;
  }
  // run the intersect algo from sfidom partially
  int current = blk_later;
  while (node2rpost.ptr[current] > node2rpost.ptr[blk_earlier]) {
    current = sfidoms_ptr[current];
  }
  // otherwise blk_earlier's dom set is not a prefix of ours - ie. it doesn't dominate us
  return current == blk_earlier;
}

// pattern: %0 = instr; &0 = %0;     -> &0 = instr;
UserFunction *slot_refslot_fuse(VMState *state, UserFunction *uf) {
  FunctionBuilder builder = {0};
  builder.block_terminated = true;
  
  int *num_slot_use = calloc(sizeof(int), uf->slots);
  
  for (int i = 0; i < uf->body.blocks_len; ++i) {
#define CHKSLOT(SLOT) num_slot_use[SLOT]++;
    Instr *instr_cur = BLOCK_START(uf, i), *instr_end = BLOCK_END(uf, i);
    while (instr_cur != instr_end) {
#define CASE(KEY, TY) } break; case KEY: { TY *instr = (TY*) instr_cur; (void) instr;
      switch (instr_cur->type) {
        case INSTR_INVALID: { abort();
#include "vm/slots.txt"
          CASE(INSTR_LAST, Instr) abort();
        } break;
        default: assert("Unhandled Instruction Type!" && false);
      }
#undef CASE
#undef CHKSLOT
      instr_cur = (Instr*) ((char*) instr_cur + instr_size(instr_cur));
    }
  }
  
  for (int i = 0; i < uf->body.blocks_len; ++i) {
    new_block(&builder);
    
    Instr *instr_cur = BLOCK_START(uf, i), *instr_end = BLOCK_END(uf, i);
    while (instr_cur != instr_end) {
      if (instr_cur->type == INSTR_CALL) {
        CallInstr *instr = (CallInstr*) instr_cur;
        if (instr->info.target.kind == ARG_SLOT && num_slot_use[instr->info.target.slot] == 2) {
          int size = sizeof(CallInstr) + sizeof(Arg) * instr->info.args_len;
          Instr *instr_next = (Instr*) ((char*) instr_cur + size);
          MoveInstr *mi_next = (MoveInstr*) instr_next;
          if (instr_next->type == INSTR_MOVE
            && mi_next->source.kind == ARG_SLOT && mi_next->source.slot == instr->info.target.slot
          ) {
            CallInstr *ci = alloca(size);
            *ci = *instr;
            for (int k = 0; k < instr->info.args_len; ++k) {
              ((Arg*)(&ci->info + 1))[k] = ((Arg*)(&instr->info + 1))[k];
            }
            ci->info.target = mi_next->target;
            addinstr_like(&builder, instr_cur, size, (Instr*)ci);
            instr_cur = (Instr*) (mi_next + 1);
            continue;
          }
        }
      }
      
      if (instr_cur->type == INSTR_ACCESS_STRING_KEY) {
        AccessStringKeyInstr *instr = (AccessStringKeyInstr*) instr_cur;
        if (instr->target.kind == ARG_SLOT && num_slot_use[instr->target.slot] == 2) {
          Instr *instr_next = (Instr*) ((char*) instr_cur + sizeof(AccessStringKeyInstr));
          MoveInstr *mi_next = (MoveInstr*) instr_next;
          if (instr_next->type == INSTR_MOVE
            && mi_next->source.kind == ARG_SLOT && mi_next->source.slot == instr->target.slot
          ) {
            AccessStringKeyInstr aski = *instr;
            aski.target = mi_next->target;
            addinstr_like(&builder, instr_cur, sizeof(aski), (Instr*)&aski);
            instr_cur = (Instr*) (mi_next + 1);
            continue;
          }
        }
      }
      
      int sz = instr_size(instr_cur);
      addinstr_like(&builder, instr_cur, sz, (Instr*) instr_cur);
      instr_cur = (Instr*) ((char*) instr_cur + sz);
    }
  }
  
  UserFunction *fn = build_function(&builder);
  copy_fn_stats(uf, fn);
  free_function(uf);
  return fn;
}

UserFunction *inline_constant_slots(VMState *state, UserFunction *uf) {
  FunctionBuilder builder = {0};
  builder.block_terminated = true;
  Value *constant_slots = find_constant_slots(uf);
  int *refslots = find_refslots(uf);
  
  for (int i = 0; i < uf->body.blocks_len; ++i) {
    new_block(&builder);
    
    Instr *instr_cur = BLOCK_START(uf, i), *instr_end = BLOCK_END(uf, i);
    while (instr_cur != instr_end) {
      if (instr_cur->type == INSTR_CALL) {
        CallInstr *instr = (CallInstr*) instr_cur;
        Arg fn = instr->info.fn;
        if (fn.kind == ARG_SLOT) {
          if (NOT_NULL(constant_slots[fn.slot])) {
            fn = (Arg) { .kind = ARG_VALUE, .value = constant_slots[fn.slot] };
          } else if (refslots[fn.slot] != -1) {
            fn = (Arg) { .kind = ARG_REFSLOT, .refslot = refslots[fn.slot] };
          }
        }
        Arg this_arg = instr->info.this_arg;
        if (this_arg.kind == ARG_SLOT) {
          if (NOT_NULL(constant_slots[this_arg.slot])) {
            this_arg = (Arg) { .kind = ARG_VALUE, .value = constant_slots[this_arg.slot] };
          } else if (refslots[this_arg.slot] != -1) {
            this_arg = (Arg) { .kind = ARG_REFSLOT, .refslot = refslots[this_arg.slot] };
          }
        }
        WriteArg target = instr->info.target;
        if (target.kind == ARG_SLOT) {
          if (NOT_NULL(constant_slots[target.slot])) {
            fprintf(stderr, "bad bytecode - call and store in .. value??\n");
            abort();
          } else if (refslots[target.slot] != -1) {
            abort(); // TODO
            target = (WriteArg) { .kind = ARG_REFSLOT, .refslot = refslots[target.slot] };
          }
        }
        int size = sizeof(CallInstr) + sizeof(Arg) * instr->info.args_len;
        CallInstr *ci = alloca(size);
        *ci = *instr;
        ci->info.target = target;
        ci->info.this_arg = this_arg;
        ci->info.fn = fn;
        for (int k = 0; k < instr->info.args_len; ++k) {
          Arg arg = ((Arg*)(&instr->info + 1))[k];
          if (arg.kind == ARG_SLOT) {
            if (NOT_NULL(constant_slots[arg.slot])) {
              arg = (Arg) { .kind = ARG_VALUE, .value = constant_slots[arg.slot] };
            } else if (refslots[arg.slot] != -1) {
              arg = (Arg) { .kind = ARG_REFSLOT, .refslot = refslots[arg.slot] };
            }
          }
          ((Arg*)(&ci->info + 1))[k] = arg;
        }
        addinstr_like(&builder, instr_cur, size, (Instr*)ci);
        instr_cur = (Instr*) ((char*) instr_cur + instr_size(instr_cur));
        continue;
      }
      
      if (instr_cur->type == INSTR_KEY_IN_OBJ) {
        KeyInObjInstr instr = *(KeyInObjInstr*) instr_cur;
        if (instr.key.kind == ARG_SLOT) {
          if (NOT_NULL(constant_slots[instr.key.slot])) {
            instr.key = (Arg) { .kind = ARG_VALUE, .value = constant_slots[instr.key.slot] };
          } else if (refslots[instr.key.slot] != -1) {
            instr.key = (Arg) { .kind = ARG_REFSLOT, .refslot = refslots[instr.key.slot] };
          }
        }
        if (instr.obj.kind == ARG_SLOT) {
          if (NOT_NULL(constant_slots[instr.obj.slot])) {
            instr.obj = (Arg) { .kind = ARG_VALUE, .value = constant_slots[instr.obj.slot] };
          } else if (refslots[instr.obj.slot] != -1) {
            instr.obj = (Arg) { .kind = ARG_REFSLOT, .refslot = refslots[instr.obj.slot] };
          }
        }
        if (instr.target.kind == ARG_SLOT) {
          if (NOT_NULL(constant_slots[instr.target.slot])) {
            fprintf(stderr, "bad bytecode - access key and store in .. value??\n");
            abort();
          } else if (refslots[instr.target.slot] != -1) {
            abort(); // TODO
            instr.target = (WriteArg) { .kind = ARG_REFSLOT, .refslot = refslots[instr.target.slot] };
          }
        }
        addinstr_like(&builder, instr_cur, sizeof(instr), (Instr*) &instr);
        instr_cur = (Instr*) ((char*) instr_cur + sizeof(instr));
        continue;
      }
      
      if (instr_cur->type == INSTR_ACCESS_STRING_KEY) {
        AccessStringKeyInstr instr = *(AccessStringKeyInstr*) instr_cur;
        if (instr.obj.kind == ARG_SLOT) {
          if (NOT_NULL(constant_slots[instr.obj.slot])) {
            instr.obj = (Arg) { .kind = ARG_VALUE, .value = constant_slots[instr.obj.slot] };
          } else if (refslots[instr.obj.slot] != -1) {
            instr.obj = (Arg) { .kind = ARG_REFSLOT, .refslot = refslots[instr.obj.slot] };
          }
        }
        if (instr.target.kind == ARG_SLOT) {
          if (NOT_NULL(constant_slots[instr.target.slot])) {
            fprintf(stderr, "bad bytecode - access key and store in .. value??\n");
            abort();
          } else if (refslots[instr.target.slot] != -1) {
            abort(); // TODO
            instr.target = (WriteArg) { .kind = ARG_REFSLOT, .refslot = refslots[instr.target.slot] };
          }
        }
        addinstr_like(&builder, instr_cur, sizeof(instr), (Instr*) &instr);
        instr_cur = (Instr*) ((char*) instr_cur + sizeof(instr));
        continue;
      }
      
      if (instr_cur->type == INSTR_MOVE) {
        MoveInstr instr = *(MoveInstr*) instr_cur;
        if (instr.source.kind == ARG_SLOT) {
          if (NOT_NULL(constant_slots[instr.source.slot])) {
            instr.source = (Arg) { .kind = ARG_VALUE, .value = constant_slots[instr.source.slot] };
          } else if (refslots[instr.source.slot] != -1) {
            instr.source = (Arg) { .kind = ARG_REFSLOT, .refslot = refslots[instr.source.slot] };
          }
        }
        addinstr_like(&builder, instr_cur, sizeof(instr), (Instr*) &instr);
        instr_cur = (Instr*) ((char*) instr_cur + sizeof(instr));
        continue;
      }
      
      if (instr_cur->type == INSTR_TESTBR) {
        TestBranchInstr instr = *(TestBranchInstr*) instr_cur;
        if (instr.test.kind == ARG_SLOT) {
          if (NOT_NULL(constant_slots[instr.test.slot])) {
            instr.test = (Arg) { .kind = ARG_VALUE, .value = constant_slots[instr.test.slot] };
          } else if (refslots[instr.test.slot] != -1) {
            instr.test = (Arg) { .kind = ARG_REFSLOT, .refslot = refslots[instr.test.slot] };
          }
        }
        addinstr_like(&builder, instr_cur, sizeof(instr), (Instr*) &instr);
        instr_cur = (Instr*) ((char*) instr_cur + sizeof(instr));
        continue;
      }
      
      if (instr_cur->type == INSTR_PHI) {
        PhiInstr instr = *(PhiInstr*) instr_cur;
        if (instr.target.kind == ARG_SLOT) {
          if (NOT_NULL(constant_slots[instr.target.slot])) {
            fprintf(stderr, "bad bytecode - compute phi node and store in .. value??\n");
            abort();
          } else if (refslots[instr.target.slot] != -1) {
            abort(); // TODO
            instr.target = (WriteArg) { .kind = ARG_REFSLOT, .refslot = refslots[instr.target.slot] };
          }
        }
        if (instr.arg1.kind == ARG_SLOT) {
          if (NOT_NULL(constant_slots[instr.arg1.slot])) {
            instr.arg1 = (Arg) { .kind = ARG_VALUE, .value = constant_slots[instr.arg1.slot] };
          } else if (refslots[instr.arg1.slot] != -1) {
            instr.arg1 = (Arg) { .kind = ARG_REFSLOT, .refslot = refslots[instr.arg1.slot] };
          }
        }
        if (instr.arg2.kind == ARG_SLOT) {
          if (NOT_NULL(constant_slots[instr.arg2.slot])) {
            instr.arg2 = (Arg) { .kind = ARG_VALUE, .value = constant_slots[instr.arg2.slot] };
          } else if (refslots[instr.arg2.slot] != -1) {
            instr.arg2 = (Arg) { .kind = ARG_REFSLOT, .refslot = refslots[instr.arg2.slot] };
          }
        }
        addinstr_like(&builder, instr_cur, sizeof(instr), (Instr*) &instr);
        instr_cur = (Instr*) ((char*) instr_cur + sizeof(instr));
        continue;
      }
      
      if (instr_cur->type == INSTR_RETURN) {
        ReturnInstr instr = *(ReturnInstr*) instr_cur;
        if (instr.ret.kind == ARG_SLOT) {
          if (NOT_NULL(constant_slots[instr.ret.slot])) {
            instr.ret = (Arg) { .kind = ARG_VALUE, .value = constant_slots[instr.ret.slot] };
          } else if (refslots[instr.ret.slot] != -1) {
            instr.ret = (Arg) { .kind = ARG_REFSLOT, .refslot = refslots[instr.ret.slot] };
          }
        }
        addinstr_like(&builder, instr_cur, sizeof(instr), (Instr*) &instr);
        instr_cur = (Instr*) ((char*) instr_cur + sizeof(instr));
        continue;
      }
      
      if (instr_cur->type == INSTR_SET_CONSTRAINT_STRING_KEY) {
        SetConstraintStringKeyInstr instr = *(SetConstraintStringKeyInstr*) instr_cur;
        if (instr.obj.kind == ARG_SLOT) {
          if (NOT_NULL(constant_slots[instr.obj.slot])) {
            instr.obj = (Arg) { .kind = ARG_VALUE, .value = constant_slots[instr.obj.slot] };
          } else if (refslots[instr.obj.slot] != -1) {
            instr.obj = (Arg) { .kind = ARG_REFSLOT, .refslot = refslots[instr.obj.slot] };
          }
        }
        if (instr.constraint.kind == ARG_SLOT) {
          if (NOT_NULL(constant_slots[instr.constraint.slot])) {
            instr.constraint = (Arg) { .kind = ARG_VALUE, .value = constant_slots[instr.constraint.slot] };
          } else if (refslots[instr.constraint.slot] != -1) {
            instr.constraint = (Arg) { .kind = ARG_REFSLOT, .refslot = refslots[instr.constraint.slot] };
          }
        }
        addinstr_like(&builder, instr_cur, sizeof(instr), (Instr*) &instr);
        instr_cur = (Instr*) ((char*) instr_cur + sizeof(instr));
        continue;
      }
      
      if (instr_cur->type == INSTR_ACCESS) {
        AccessInstr instr = *(AccessInstr*) instr_cur;
        if (instr.obj.kind == ARG_SLOT) {
          if (NOT_NULL(constant_slots[instr.obj.slot])) {
            instr.obj = (Arg) { .kind = ARG_VALUE, .value = constant_slots[instr.obj.slot] };
          } else if (refslots[instr.obj.slot] != -1) {
            instr.obj = (Arg) { .kind = ARG_REFSLOT, .refslot = refslots[instr.obj.slot] };
          }
        }
        if (instr.target.kind == ARG_SLOT) {
          if (NOT_NULL(constant_slots[instr.target.slot])) {
            fprintf(stderr, "bad bytecode - access key and store in .. value??\n");
            abort();
          } else if (refslots[instr.target.slot] != -1) {
            abort(); // TODO
            instr.target = (WriteArg) { .kind = ARG_REFSLOT, .refslot = refslots[instr.target.slot] };
          }
        }
        if (instr.key.kind == ARG_SLOT) {
          if (NOT_NULL(constant_slots[instr.key.slot])) {
            instr.key = (Arg) { .kind = ARG_VALUE, .value = constant_slots[instr.key.slot] };
          } else if (refslots[instr.key.slot] != -1) {
            instr.key = (Arg) { .kind = ARG_REFSLOT, .refslot = refslots[instr.key.slot] };
          }
        }
        addinstr_like(&builder, instr_cur, sizeof(instr), (Instr*) &instr);
        instr_cur = (Instr*) ((char*) instr_cur + sizeof(instr));
        continue;
      }
      
      if (instr_cur->type == INSTR_INSTANCEOF) {
        InstanceofInstr instr = *(InstanceofInstr*) instr_cur;
        if (instr.obj.kind == ARG_SLOT) {
          if (NOT_NULL(constant_slots[instr.obj.slot])) {
            instr.obj = (Arg) { .kind = ARG_VALUE, .value = constant_slots[instr.obj.slot] };
          } else if (refslots[instr.obj.slot] != -1) {
            instr.obj = (Arg) { .kind = ARG_REFSLOT, .refslot = refslots[instr.obj.slot] };
          }
        }
        if (instr.proto.kind == ARG_SLOT) {
          if (NOT_NULL(constant_slots[instr.proto.slot])) {
            instr.proto = (Arg) { .kind = ARG_VALUE, .value = constant_slots[instr.proto.slot] };
          } else if (refslots[instr.proto.slot] != -1) {
            instr.proto = (Arg) { .kind = ARG_REFSLOT, .refslot = refslots[instr.proto.slot] };
          }
        }
        if (instr.target.kind == ARG_SLOT) {
          if (NOT_NULL(constant_slots[instr.target.slot])) {
            fprintf(stderr, "bad bytecode - instanceof and store in .. value??\n");
            abort();
          } else if (refslots[instr.target.slot] != -1) {
            abort(); // TODO
            instr.target = (WriteArg) { .kind = ARG_REFSLOT, .refslot = refslots[instr.target.slot] };
          }
        }
        addinstr_like(&builder, instr_cur, sizeof(instr), (Instr*) &instr);
        instr_cur = (Instr*) ((char*) instr_cur + sizeof(instr));
        continue;
      }
      
      if (instr_cur->type == INSTR_ASSIGN) {
        AssignInstr instr = *(AssignInstr*) instr_cur;
        if (instr.obj.kind == ARG_SLOT) {
          if (NOT_NULL(constant_slots[instr.obj.slot])) {
            instr.obj = (Arg) { .kind = ARG_VALUE, .value = constant_slots[instr.obj.slot] };
          } else if (refslots[instr.obj.slot] != -1) {
            instr.obj = (Arg) { .kind = ARG_REFSLOT, .refslot = refslots[instr.obj.slot] };
          }
        }
        if (instr.value.kind == ARG_SLOT) {
          if (NOT_NULL(constant_slots[instr.value.slot])) {
            instr.value = (Arg) { .kind = ARG_VALUE, .value = constant_slots[instr.value.slot] };
          } else if (refslots[instr.value.slot] != -1) {
            instr.value = (Arg) { .kind = ARG_REFSLOT, .refslot = refslots[instr.value.slot] };
          }
        }
        if (instr.key.kind == ARG_SLOT) {
          if (NOT_NULL(constant_slots[instr.key.slot])) {
            instr.key = (Arg) { .kind = ARG_VALUE, .value = constant_slots[instr.key.slot] };
          } else if (refslots[instr.key.slot] != -1) {
            instr.key = (Arg) { .kind = ARG_REFSLOT, .refslot = refslots[instr.key.slot] };
          }
        }
        addinstr_like(&builder, instr_cur, sizeof(instr), (Instr*) &instr);
        instr_cur = (Instr*) ((char*) instr_cur + sizeof(instr));
        continue;
      }
      
      if (instr_cur->type == INSTR_ASSIGN_STRING_KEY) {
        AssignStringKeyInstr instr = *(AssignStringKeyInstr*) instr_cur;
        if (instr.obj.kind == ARG_SLOT) {
          if (NOT_NULL(constant_slots[instr.obj.slot])) {
            instr.obj = (Arg) { .kind = ARG_VALUE, .value = constant_slots[instr.obj.slot] };
          } else if (refslots[instr.obj.slot] != -1) {
            instr.obj = (Arg) { .kind = ARG_REFSLOT, .refslot = refslots[instr.obj.slot] };
          }
        }
        if (instr.value.kind == ARG_SLOT) {
          if (NOT_NULL(constant_slots[instr.value.slot])) {
            instr.value = (Arg) { .kind = ARG_VALUE, .value = constant_slots[instr.value.slot] };
          } else if (refslots[instr.value.slot] != -1) {
            instr.value = (Arg) { .kind = ARG_REFSLOT, .refslot = refslots[instr.value.slot] };
          }
        }
        addinstr_like(&builder, instr_cur, sizeof(instr), (Instr*) &instr);
        instr_cur = (Instr*) ((char*) instr_cur + sizeof(instr));
        continue;
      }
      
      addinstr_like(&builder, instr_cur, instr_size(instr_cur), instr_cur);
      instr_cur = (Instr*) ((char*) instr_cur + instr_size(instr_cur));
    }
  }
  UserFunction *fn = build_function(&builder);
  copy_fn_stats(uf, fn);
  free_function(uf);
  return fn;
}

UserFunction *fuse_static_object_alloc(VMState *state, UserFunction *uf) {
  FunctionBuilder builder = {0};
  builder.block_terminated = true;
  
  Value *constant_slots = find_constant_slots(uf);
  
  for (int i = 0; i < uf->body.blocks_len; ++i) {
    new_block(&builder);
    
    Instr *instr = BLOCK_START(uf, i), *instr_end = BLOCK_END(uf, i);
    while (instr != instr_end) {
      if (instr->type == INSTR_ALLOC_OBJECT) {
        AllocObjectInstr *alobi = (AllocObjectInstr*) instr;
        bool failed = false;
        StaticFieldInfo *info_ptr = NULL; int info_len = 0;
        int refslots_set = 0;
        bool closed = false;
        
        Instr *instr_reading = (Instr*) (alobi + 1);
        while (instr_reading != instr_end) {
          if (instr_reading->type == INSTR_ASSIGN_STRING_KEY) {
            if (closed) { failed = true; break; }
            AssignStringKeyInstr *aski = (AssignStringKeyInstr*) instr_reading;
            if (aski->type != ASSIGN_PLAIN) { failed = true; break; }
            if (aski->obj.kind != ARG_SLOT) { failed = true; break; }
            if (aski->value.kind != ARG_SLOT) { failed = true; break; }
            if (aski->obj.slot != alobi->target_slot) { failed = true; break; }
            
            info_ptr = realloc(info_ptr, sizeof(StaticFieldInfo) * ++info_len);
            info_ptr[info_len - 1] = (StaticFieldInfo) {0};
            StaticFieldInfo *info = &info_ptr[info_len - 1];
            info->name_ptr = aski->key;
            info->name_len = strlen(aski->key);
            info->name_hash = hash(info->name_ptr, info->name_len);
            info->slot = aski->value.slot;
            info->refslot = -1;
            
            instr_reading = (Instr*) (aski + 1);
          } else if (instr_reading->type == INSTR_CLOSE_OBJECT) {
            CloseObjectInstr *coi = (CloseObjectInstr*) instr_reading;
            closed = true;
            instr_reading = (Instr*) (coi + 1);
            if (refslots_set == info_len) break; // all refslots already assigned
          } else if (instr_reading->type == INSTR_DEFINE_REFSLOT) {
            DefineRefslotInstr *dri = (DefineRefslotInstr*) instr_reading;
            for (int k = 0; k < info_len; ++k) {
              StaticFieldInfo *info = &info_ptr[k];
              if (info->name_len == dri->key_len && strncmp(info->name_ptr, dri->key_ptr, info->name_len) == 0) {
                if (info->refslot != -1) {
                  failed = true;
                  break; // wat wat wat
                }
                info->refslot = dri->target_refslot;
                refslots_set ++;
              }
            }
            instr_reading = (Instr*) (dri + 1);
            if (closed && refslots_set == info_len) break; // all refslots assigned
          } else {
            failed = true;
            // fprintf(stderr, "failed slot %i because %i\n", alobi->target_slot, instr->type);
            break;
          }
        }
        if (!failed) {
          while (instr_reading != instr_end && instr_reading->type == INSTR_SET_CONSTRAINT_STRING_KEY
            && ((SetConstraintStringKeyInstr*)instr_reading)->obj.kind == ARG_SLOT)
          {
            SetConstraintStringKeyInstr *scski = (SetConstraintStringKeyInstr*) instr_reading;
            
            if (scski->obj.slot != alobi->target_slot) break;
            if (scski->constraint.kind != ARG_SLOT && scski->constraint.kind != ARG_VALUE) break;
            if (scski->constraint.kind == ARG_SLOT && !IS_OBJ(constant_slots[scski->constraint.slot])) break; // wat wat
            
            for (int k = 0; k < info_len; ++k) {
              StaticFieldInfo *info = &info_ptr[k];
              if (info->name_len == scski->key_len && strncmp(info->name_ptr, scski->key_ptr, info->name_len) == 0) {
                if (info->constraint) abort(); // wat wat wat
                if (scski->constraint.kind == ARG_SLOT) info->constraint = AS_OBJ(constant_slots[scski->constraint.slot]);
                else info->constraint = AS_OBJ(scski->constraint.value);
              }
            }
            
            instr_reading = (Instr*) (scski + 1);
          }
          Object sample_obj = {0};
          for (int k = 0; k < info_len; ++k) {
            StaticFieldInfo *info = &info_ptr[k];
            char *error = object_set(state, &sample_obj, info->name_ptr, VNULL);
            if (error) { fprintf(stderr, "INTERNAL LOGIC ERROR: %s\n", error); abort(); }
          }
          for (int k = 0; k < info_len; ++k) {
            StaticFieldInfo *info = &info_ptr[k];
            TableEntry *entry = table_lookup_with_hash(&sample_obj.tbl, info->name_ptr, info->name_len, info->name_hash);
            if (!entry) { fprintf(stderr, "where has it gone??\n"); abort(); }
            info->tbl_offset = entry - sample_obj.tbl.entries_ptr;
          }
          
          AllocStaticObjectInstr *asoi = alloca(sizeof(AllocStaticObjectInstr)
                                                + sizeof(Object)
                                                + sizeof(StaticFieldInfo) * info_len);
          *asoi = (AllocStaticObjectInstr) {
            .base = { .type = INSTR_ALLOC_STATIC_OBJECT },
            .info_len = info_len,
            .parent_slot = alobi->parent_slot,
            .target_slot = alobi->target_slot,
          };
          ASOI_OBJ(asoi) = sample_obj;
          for (int k = 0; k < info_len; ++k) {
            ASOI_INFO(asoi)[k] = info_ptr[k];
          }
          addinstr_like(&builder, instr, instr_size((Instr*) asoi), (Instr*) asoi);
          instr = instr_reading;
          continue;
        }
      }
      
      addinstr_like(&builder, instr, instr_size(instr), instr);
      instr = (Instr*) ((char*) instr + instr_size(instr));
    }
  }
  UserFunction *fn = build_function(&builder);
  copy_fn_stats(uf, fn);
  free_function(uf);
  return fn;
}

static void reassign_slot(int *slot_p, int numslots, Instr *instr_cur, Instr **first_use, Instr **last_use, bool *newslot_in_use, int *slot_map) {
  int slot = *slot_p;
  if (instr_cur == first_use[slot]) {
    int selected_slot = -1;
    for (int k = 0; k < numslots; ++k) if (!newslot_in_use[k]) {
      selected_slot = k;
      break;
    }
    assert(selected_slot != -1);
    newslot_in_use[selected_slot] = true;
    slot_map[slot] = selected_slot;
  }
  *slot_p = slot_map[slot];
  if (instr_cur == last_use[slot]) {
    newslot_in_use[slot_map[slot]] = false;
  }
}

// WARNING
// this function makes the IR **NON-SSA**
// and thus it **MUST** come completely last!!
UserFunction *compactify_registers(UserFunction *uf) {
  FunctionBuilder builder = {0};
  builder.block_terminated = true;
  
  // use the lexical orderedness assumption
  Instr **first_use = calloc(sizeof(Instr*), uf->slots);
  Instr **last_use = calloc(sizeof(Instr*), uf->slots);
  
  for (int i = 0; i < uf->body.blocks_len; ++i) {
#define CHKSLOT(VAL) if (!first_use[VAL]) first_use[VAL] = instr_cur; last_use[VAL] = instr_cur
    Instr *instr_cur = BLOCK_START(uf, i), *instr_end = BLOCK_END(uf, i);
    while (instr_cur != instr_end) {
#define CASE(KEY, TY) } break; case KEY: { TY *instr = (TY*) instr_cur; (void) instr;
      switch (instr_cur->type) {
        case INSTR_INVALID: { abort();
#include "vm/slots.txt"
          CASE(INSTR_LAST, Instr) abort();
        } break;
        default: assert("Unhandled Instruction Type!" && false);
      }
#undef CASE
#undef CHKSLOT
      instr_cur = (Instr*) ((char*) instr_cur + instr_size(instr_cur));
    }
  }
  
  bool *newslot_in_use = calloc(sizeof(bool), uf->slots);
  int *slot_map = calloc(sizeof(int), uf->slots);
  
  // specials
  for (int i = 0; i < 2 + uf->arity; ++i) {
    first_use[i] = NULL;
    last_use[i] = NULL;
    newslot_in_use[i] = true;
    slot_map[i] = i;
  }
  
  for (int i = 0; i < uf->body.blocks_len; ++i) {
    new_block(&builder);
    
    Instr *instr_cur = BLOCK_START(uf, i), *instr_end = BLOCK_END(uf, i);
    while (instr_cur != instr_end) {
      switch (instr_cur->type) {

#define CASE(KEY, TY) \
          use_range_start(&builder, instr_cur->belongs_to);\
          builder.scope = ((Instr*) instr)->context_slot;\
          addinstr(&builder, sz, (Instr*) instr);\
          use_range_end(&builder, instr_cur->belongs_to);\
          instr_cur = (Instr*) ((char*) instr_cur + sz);\
          continue;\
        }\
        case KEY: {\
          int sz = instr_size(instr_cur);\
          TY *instr = (TY*) alloca(sz);\
          memcpy(instr, instr_cur, sz);

#define CHKSLOT(S) reassign_slot(&S, uf->slots, instr_cur, first_use, last_use, newslot_in_use, slot_map)

        case INSTR_INVALID: { abort(); Instr *instr = NULL; int sz = 0;
#include "vm/slots.txt"
          CASE(INSTR_LAST, Instr) abort();
        } break;
        default: assert("Unhandled Instruction Type!" && false);

#undef CHKSLOT
#undef CASE
      }
    }
  }
  
  int maxslot = 0;
  for (int i = 0; i < uf->slots; ++i) if (slot_map[i] > maxslot) maxslot = slot_map[i];
  
  UserFunction *fn = build_function(&builder);
  copy_fn_stats(uf, fn);
  free_function(uf);
  fn->slots = maxslot + 1;
  fn->non_ssa = true;
  return fn;
}

UserFunction *remove_pointless_blocks(UserFunction *uf) {
  CFG cfg;
  cfg_build(&cfg, uf);
  
  bool *blk_live = calloc(sizeof(bool), uf->body.blocks_len);
  blk_live[0] = true;
  
  bool changed;
  do {
    changed = false;
    // yes yes, you can construct a pathological graph of blocks, who cares
    // the language doesn't have goto anyways
    for (int i = 0; i < uf->body.blocks_len; ++i) {
      if (blk_live[i]) {
        for (int k = 0; k < cfg.nodes_ptr[i].succ_len; ++k) {
          bool *liveflag = &blk_live[cfg.nodes_ptr[i].succ_ptr[k]];
          if (!*liveflag) {
            *liveflag = true;
            changed = true;
          }
        }
      }
    }
  } while (changed);
  
  int *blk_map = calloc(sizeof(int), uf->body.blocks_len);
  
  int k = 0;
  for (int i = 0; i < uf->body.blocks_len; ++i) {
    if (blk_live[i]) blk_map[i] = k++;
    else blk_map[i] = -1;
  }
  
  FunctionBuilder builder = {0};
  builder.block_terminated = true;
  
  for (int i = 0; i < uf->body.blocks_len; ++i) {
    if (!blk_live[i]) continue;
    
    new_block(&builder);
    Instr *instr_cur = BLOCK_START(uf, i), *instr_end = BLOCK_END(uf, i);
    while (instr_cur != instr_end) {
      if (instr_cur->type == INSTR_BR) {
        BranchInstr *instr = (BranchInstr*) instr_cur;
        instr_cur = (Instr*) (instr + 1);
        BranchInstr bri = *instr;
        bri.blk = blk_map[bri.blk];
        addinstr_like(&builder, instr_cur, sizeof(bri), (Instr*) &bri);
        continue;
      }
      if (instr_cur->type == INSTR_TESTBR) {
        TestBranchInstr *instr = (TestBranchInstr*) instr_cur;
        instr_cur = (Instr*) (instr + 1);
        TestBranchInstr tbri = *instr;
        tbri.true_blk = blk_map[tbri.true_blk];
        tbri.false_blk = blk_map[tbri.false_blk];
        addinstr_like(&builder, instr_cur, sizeof(tbri), (Instr*) &tbri);
        continue;
      }
      if (instr_cur->type == INSTR_PHI) {
        PhiInstr *instr = (PhiInstr*) instr_cur;
        instr_cur = (Instr*) (instr + 1);
        PhiInstr phi = *instr;
        phi.block1 = blk_map[phi.block1];
        phi.block2 = blk_map[phi.block2];
        addinstr_like(&builder, instr_cur, sizeof(phi), (Instr*) &phi);
        continue;
      }
      addinstr_like(&builder, instr_cur, instr_size(instr_cur), instr_cur);
      instr_cur = (Instr*) ((char*) instr_cur + instr_size(instr_cur));
    }
  }
  
  cfg_destroy(&cfg);
  free(blk_live);
  free(blk_map);
  
  UserFunction *fn = build_function(&builder);
  copy_fn_stats(uf, fn);
  free_function(uf);
  return fn;
}

UserFunction *optimize_runtime(VMState *state, UserFunction *uf, Object *context) {
  if (uf->non_ssa != false) {
    fprintf(stderr, "called optimizer on function that is non-ssa!\n");
    abort();
  }
  /*
  printf("runtime optimize %s with %p\n", uf->name, (void*) context);
  if (uf->name && strcmp(uf->name, "gear") == 0) print_recursive(state, context, false);
  printf("\n-----\n");
  */
  
  // moved here because it can be kind of expensive due to lazy coding, and I'm too lazy to fix it
  uf = inline_static_lookups_to_constants(state, uf, context, false);
  // run a second time, to pick up accesses on objects that just now became statically known
  uf = inline_static_lookups_to_constants(state, uf, context, true);
  
  uf = access_vars_via_refslots(uf);
  
  uf = inline_constant_slots(state, uf);
  
  // run a third time, to pick up on instanceof patterns
  uf = inline_static_lookups_to_constants(state, uf, context, true);
  uf = inline_constant_slots(state, uf);
  
  uf = slot_refslot_fuse(state, uf);
  
  // must be late!
  uf = fuse_static_object_alloc(state, uf);
  uf = remove_dead_slot_writes(uf);
  uf = call_functions_directly(state, uf);
  
  uf = remove_pointless_blocks(uf);
  
  // must be very very *very* last!
  uf = compactify_registers(uf);
  
  uf->optimized = true; // will be optimized no further
  
  if (state->shared->verbose) {
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
    int *sfidoms_ptr = cfg_build_sfidom_list(&cfg, rpost2node, node2rpost);
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
  
  if (state->shared->verbose) {
    fprintf(stderr, "runtime optimized %s to\n", uf->name);
    dump_fn(state, uf);
  }
  
  return uf;
}
