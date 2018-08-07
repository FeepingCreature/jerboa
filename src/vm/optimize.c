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
          CASE(INSTR_ALLOC_OBJECT, AllocObjectInstr)
            slots[slot_index(instr->parent_slot)] = false;
          CASE(INSTR_ALLOC_INT_OBJECT, AllocIntObjectInstr)
          CASE(INSTR_ALLOC_BOOL_OBJECT, AllocBoolObjectInstr)
          CASE(INSTR_ALLOC_FLOAT_OBJECT, AllocFloatObjectInstr)
          CASE(INSTR_ALLOC_ARRAY_OBJECT, AllocArrayObjectInstr)
          CASE(INSTR_ALLOC_STRING_OBJECT, AllocStringObjectInstr)
          CASE(INSTR_ALLOC_CLOSURE_OBJECT, AllocClosureObjectInstr)
          CASE(INSTR_CLOSE_OBJECT, CloseObjectInstr)
          CASE(INSTR_FREEZE_OBJECT, FreezeObjectInstr)
          CASE(INSTR_ACCESS, AccessInstr)
            if (instr->obj.kind == ARG_SLOT) slots[slot_index(instr->obj.slot)] = false;
          CASE(INSTR_ASSIGN, AssignInstr)
            if (instr->obj.kind == ARG_SLOT) slots[slot_index(instr->obj.slot)] = false;
            if (instr->value.kind == ARG_SLOT) slots[slot_index(instr->value.slot)] = false;
          // TODO inline key?
          CASE(INSTR_KEY_IN_OBJ, KeyInObjInstr)
            if (instr->obj.kind == ARG_SLOT) slots[slot_index(instr->obj.slot)] = false;
          CASE(INSTR_IDENTICAL, IdenticalInstr)
            if (instr->obj1.kind == ARG_SLOT) slots[slot_index(instr->obj1.slot)] = false;
            if (instr->obj2.kind == ARG_SLOT) slots[slot_index(instr->obj2.slot)] = false;
          CASE(INSTR_INSTANCEOF, InstanceofInstr)
            if (instr->obj.kind == ARG_SLOT) slots[slot_index(instr->obj.slot)] = false;
            if (instr->proto.kind == ARG_SLOT) slots[slot_index(instr->proto.slot)] = false;
          CASE(INSTR_SET_CONSTRAINT, SetConstraintInstr)
            if (instr->obj.kind == ARG_SLOT) slots[slot_index(instr->obj.slot)] = false;
            if (instr->constraint.kind == ARG_SLOT) slots[slot_index(instr->constraint.slot)] = false;
          CASE(INSTR_TEST, TestInstr)
          CASE(INSTR_CALL, CallInstr)
            if (instr->info.fn.kind == ARG_SLOT) slots[slot_index(instr->info.fn.slot)] = false;
            if (instr->info.this_arg.kind == ARG_SLOT) slots[slot_index(instr->info.this_arg.slot)] = false;
            for (int k = 0; k < instr->info.args_len; ++k) {
              Arg arg = ((Arg*)(&instr->info + 1))[k];
              if (arg.kind == ARG_SLOT) slots[slot_index(arg.slot)] = false;
            }
          CASE(INSTR_RETURN, ReturnInstr)
            if (instr->ret.kind == ARG_SLOT) slots[slot_index(instr->ret.slot)] = false;
          CASE(INSTR_BR, BranchInstr)
          CASE(INSTR_TESTBR, TestBranchInstr)
            if (instr->test.kind == ARG_SLOT) slots[slot_index(instr->test.slot)] = false;
          CASE(INSTR_PHI, PhiInstr)
            if (instr->arg1.kind == ARG_SLOT) slots[slot_index(instr->arg1.slot)] = false;
            if (instr->arg2.kind == ARG_SLOT) slots[slot_index(instr->arg2.slot)] = false;
            if (instr->target.kind == ARG_SLOT) slots[slot_index(instr->target.slot)] = false;
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
          slots[slot_index_rt(uf, mi->target.slot)] = mi->source.value;
        }
      }
      instr = (Instr*)((char*) instr + instr_size(instr));
    }
  }
  return slots;
}

static Refslot *find_refslots(UserFunction *uf) {
  Refslot *slots = calloc(sizeof(Refslot), uf->slots);
  for (int i = 0; i < uf->slots; ++i) slots[i] = (Refslot) { .index = -1 };
  for (int i = 0; i < uf->body.blocks_len; ++i) {
    Instr *instr = BLOCK_START(uf, i), *instr_end = BLOCK_END(uf, i);
    while (instr != instr_end) {
      if (instr->type == INSTR_MOVE) {
        MoveInstr *mi = (MoveInstr*) instr;
        if (mi->source.kind == ARG_REFSLOT && mi->target.kind == ARG_SLOT) {
          slots[slot_index_rt(uf, mi->target.slot)] = mi->source.refslot;
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
  Slot parent_slot;

  int fields_len;
  const char **names_ptr;
  ConstraintInfo *constraints_ptr;

  FileRange *belongs_to;
  Instr *after_object_decl;
} SlotIsStaticObjInfo;

static int static_info_find_field(SlotIsStaticObjInfo *rec, const char *name_ptr) {
  assert(rec->static_object);
  for (int k = 0; k < rec->fields_len; ++k) {
    if (rec->names_ptr[k] == name_ptr) {
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
        int obj_slot = slot_index_rt(uf, dri->obj_slot);
        SlotIsStaticObjInfo *rec = &((*slots_p)[obj_slot]);
        if (rec->static_object) {
          int field = static_info_find_field(rec, dri->key.key);
          if (field != -1) {
            objslot_for_refslot[refslot_index_rt(uf, dri->target_refslot)] = obj_slot;
            field_for_refslot[refslot_index_rt(uf, dri->target_refslot)] = field;
          }
        }
        continue;
      }
      if (instr->type == INSTR_ALLOC_OBJECT) {
        AllocObjectInstr *alobi = (AllocObjectInstr*) instr;
        instr = (Instr*) (alobi + 1);
        bool failed = false;
        const char **names_ptr = NULL; int *names_len_ptr = NULL; int fields_len = 0;
        while (instr != instr_end) {
          if (instr->type == INSTR_ASSIGN_STRING_KEY) {
            AssignStringKeyInstr *aski = (AssignStringKeyInstr*) instr;
            if (aski->type != ASSIGN_PLAIN) { failed = true; break; }
            if (aski->obj.kind != ARG_SLOT) { failed = true; break; }
            if (slot_index_rt(uf, aski->obj.slot) != slot_index_rt(uf, alobi->target_slot)) { failed = true; break; }

            instr = (Instr*) (aski + 1);
            names_ptr = realloc(names_ptr, sizeof(char*) * ++fields_len);
            names_len_ptr = realloc(names_len_ptr, sizeof(int) * fields_len);
            names_ptr[fields_len - 1] = aski->key.key;
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
        int target_slot = slot_index_rt(uf, alobi->target_slot);
        (*slots_p)[target_slot].static_object = true;
        (*slots_p)[target_slot].parent_slot = alobi->parent_slot;
        (*slots_p)[target_slot].fields_len = fields_len;
        (*slots_p)[target_slot].names_ptr = names_ptr;
        (*slots_p)[target_slot].constraints_ptr = calloc(sizeof(ConstraintInfo), fields_len);
        (*slots_p)[target_slot].belongs_to = *instr_belongs_to_p(&uf->body, instr);

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
          else constraint = OBJ_OR_NULL(constant_slots[slot_index_rt(uf, scski->constraint.slot)]);
          SlotIsStaticObjInfo *rec = &(*slots_p)[slot_index_rt(uf, scski->obj.slot)];

          if (constraint && rec->static_object) {
            int field = static_info_find_field(rec, scski->key.key);
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
        if (ins->proto.kind == ARG_VALUE && IS_OBJ(ins->proto.value)
          && ins->obj.kind == ARG_REFSLOT
          && ins->target.kind == ARG_SLOT)
        {
          Slot bool_slot = ins->target.slot;

          if (instr->type == INSTR_TEST) {
            TestInstr *ti = (TestInstr*) instr;
            if (ti->value.kind == ARG_SLOT && slot_index_rt(uf, ti->value.slot) == slot_index_rt(uf, bool_slot)
              && ti->target.kind == ARG_SLOT)
            {
              bool_slot = ti->target.slot;
              instr = (Instr*) (ti + 1);
            }
          }
          if (instr->type == INSTR_TESTBR) {
            TestBranchInstr *tbr = (TestBranchInstr*) instr;
            instr = (Instr*) (tbr + 1);
            if (tbr->test.kind == ARG_SLOT && slot_index_rt(uf, bool_slot) == slot_index_rt(uf, tbr->test.slot)
              && cfg.nodes_ptr[tbr->true_blk].pred_len == 1)
            {
              assert(cfg.nodes_ptr[tbr->true_blk].pred_ptr[0] == i);
              Object *constraint = AS_OBJ(ins->proto.value);
              int refslot = refslot_index_rt(uf, ins->obj.refslot);
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
  to->body.function_range = from->body.function_range;
  to->resolved = from->resolved;
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
          int obj_slot = slot_index(aski_new.obj.slot);
          if (!info[obj_slot].static_object) break;
          int field = static_info_find_field(&info[obj_slot], aski->key.key);
          if (field != -1) break;
          // since the key was not found in obj
          // we can know statically that the lookup will
          // not succeed at runtime either
          // (since the object is closed, its keys are statically known)
          // so instead look up in the (known) parent object from the start
          aski_new.obj = (Arg) { .kind = ARG_SLOT, .slot = info[obj_slot].parent_slot };
        }
        addinstr_like(&builder, &uf->body, instr, sizeof(aski_new), (Instr*) &aski_new);
        instr = (Instr*) (aski + 1);
        continue;
      }

      if (instr->type == INSTR_ASSIGN_STRING_KEY) {
        AssignStringKeyInstr *aski = (AssignStringKeyInstr*) instr;
        if (aski->type == ASSIGN_EXISTING) {
          AssignStringKeyInstr aski_new = *aski;
          if (aski_new.obj.kind == ARG_SLOT) {
            while (true) {
              int obj_slot = slot_index(aski_new.obj.slot);
              if (!info[obj_slot].static_object) break;
              int field = static_info_find_field(&info[obj_slot], aski->key.key);
              if (field != -1) break; // key was found, we're at the right object
              aski_new.obj.slot = info[obj_slot].parent_slot;
            }
            addinstr_like(&builder, &uf->body, instr, sizeof(aski_new), (Instr*) &aski_new);
            instr = (Instr*) (aski + 1);
            continue;
          }
        }
      }

      addinstr_like(&builder, &uf->body, instr, instr_size(instr), instr);
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
  assert(uf->resolved);

  SlotIsStaticObjInfo *info;
  slot_is_static_object(uf, &info);

  int info_slots_len = 0;
  for (int i = 0; i < uf->slots; ++i) if (info[i].static_object) info_slots_len ++;
  int *info_slots_ptr = malloc(sizeof(int) * info_slots_len);
  bool *obj_refslots_initialized = calloc(sizeof(bool), uf->slots);
  Refslot **ref_slots_ptr = calloc(sizeof(int*), uf->slots);
  {
    int k = 0;
    for (int i = 0; i < uf->slots; ++i) if (info[i].static_object) {
      info_slots_ptr[k++] = i;
      ref_slots_ptr[i] = malloc(sizeof(Refslot) * info[i].fields_len);
    }
  }

  FunctionBuilder builder = {0};
  builder.refslot_base = uf->refslots;
  builder.slot_base = uf->slots;
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
          for (int l = 0; l < info[slot].fields_len; ++l) {
            Slot obj_slot = (Slot) { .index = slot };
            resolve_slot_ref(uf, &obj_slot);
            ref_slots_ptr[slot][l] = addinstr_def_refslot(&builder, obj_slot, info[slot].names_ptr[l], strlen(info[slot].names_ptr[l]));
            // bit hacky
            DefineRefslotInstr *written_instr = (DefineRefslotInstr*) ((char*) builder.body.instrs_ptr_end - sizeof(DefineRefslotInstr));
            // pretend we already track enough refslots, since we can't pass in the builder
            int backup = uf->refslots; uf->refslots = written_instr->target_refslot.index + 1;
            resolve_refslot_ref(uf, &ref_slots_ptr[slot][l]);
            resolve_refslot_ref(uf, &written_instr->target_refslot);
            uf->refslots = backup;
          }
          use_range_end(&builder, info[slot].belongs_to);
          obj_refslots_initialized[slot] = true;
        }
      }

      if (instr->type == INSTR_ACCESS_STRING_KEY) {
        AccessStringKeyInstr *aski = (AccessStringKeyInstr*) instr;
        if (aski->obj.kind == ARG_SLOT && aski->target.kind == ARG_SLOT) {
          int obj_slot = slot_index_rt(uf, aski->obj.slot);
          const char *keyptr = aski->key.key;
          if (info[obj_slot].static_object && obj_refslots_initialized[obj_slot]) {
            bool continue_outer = false;
            for (int k = 0; k < info[obj_slot].fields_len; ++k) {
              if (keyptr == info[obj_slot].names_ptr[k]) {
                Refslot refslot = ref_slots_ptr[obj_slot][k];
                use_range_start(&builder, *instr_belongs_to_p(&uf->body, instr));
                addinstr_move(&builder,
                              (Arg){.kind=ARG_REFSLOT,.refslot=refslot},
                              aski->target);
                use_range_end(&builder, *instr_belongs_to_p(&uf->body, instr));
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
          int obj_slot = slot_index_rt(uf, aski->obj.slot);
          FastKey key = aski->key;
          if (info[obj_slot].static_object && obj_refslots_initialized[obj_slot]) {
            bool continue_outer = false;
            for (int k = 0; k < info[obj_slot].fields_len; ++k) {
              if (key.key == info[obj_slot].names_ptr[k]) {
                Refslot refslot = ref_slots_ptr[obj_slot][k];
                use_range_start(&builder, *instr_belongs_to_p(&uf->body, instr));
                addinstr_move(&builder, aski->value, (WriteArg){.kind=ARG_REFSLOT,.refslot=refslot});
                use_range_end(&builder, *instr_belongs_to_p(&uf->body, instr));
                instr = (Instr*) (aski + 1);
                continue_outer = true;
                break;
              }
            }
            if (continue_outer) continue;
          }
        }
      }

      addinstr_like(&builder, &uf->body, instr, instr_size(instr), instr);
      instr = (Instr*) ((char*) instr + instr_size(instr));
    }
  }
  UserFunction *fn = build_function(&builder);
  int new_refslots = fn->refslots;
  assert(fn->slots == uf->slots);
  copy_fn_stats(uf, fn);
  free_function(uf);
  fn->refslots = new_refslots; // safe to update, can only grow

  free(info);
  free(info_slots_ptr);
  free(obj_refslots_initialized);
  for (int i = 0; i < fn->slots; ++i) {
    free(ref_slots_ptr[i]);
  }
  free(ref_slots_ptr);

  return fn;
}

UserFunction *null_this_in_thisless_calls(VMState *state, UserFunction *uf) {
  FunctionBuilder builder = {0};
  builder.block_terminated = true;

  Object *closure_base = state->shared->vcache.closure_base;

  for (int i = 0; i < uf->body.blocks_len; ++i) {
    new_block(&builder);

    Instr *instr_cur = BLOCK_START(uf, i), *instr_end = BLOCK_END(uf, i);
    while (instr_cur != instr_end) {
      int instrsz = instr_size(instr_cur);
      if (instr_cur->type == INSTR_CALL) {
        CallInstr *instr = (CallInstr*) instr_cur;
        CallInstr *instr_new = alloca(instrsz);
        memcpy(instr_new, instr, instrsz);
        if (instr->info.fn.kind == ARG_VALUE) {
          Object *fn_obj = OBJ_OR_NULL(instr->info.fn.value);
          ClosureObject *cl = (ClosureObject*) obj_instance_of(fn_obj, closure_base);
          if (cl && !cl->vmfun->is_method) {
            instr_new->info.this_arg = (Arg) {.kind = ARG_VALUE, .value = VNULL };
          }
        }
        addinstr_like(&builder, &uf->body, instr_cur, instrsz, (Instr*) instr_new);
      } else {
        addinstr_like(&builder, &uf->body, instr_cur, instrsz, (Instr*) instr_cur);
      }

      instr_cur = (Instr*) ((char*) instr_cur + instr_size(instr_cur));
    }
  }

  UserFunction *fn = build_function(&builder);
  copy_fn_stats(uf, fn);
  free_function(uf);
  return fn;
}

typedef struct {
  bool test_escapes, definitely_escapes;
  int *ptr;
  int length;
} SlotEscapeStat;

void ia_append(SlotEscapeStat *array, int value) {
  array->ptr = realloc(array->ptr, sizeof(int) * ++array->length);
  array->ptr[array->length - 1] = value;
}

bool any_escape(SlotEscapeStat *escape_stat, int slot) {
  if (escape_stat[slot].definitely_escapes) return true;
  if (!escape_stat[slot].test_escapes) return false;
  for (int i = 0; i < escape_stat[slot].length; i++) {
    if (any_escape(escape_stat, escape_stat[slot].ptr[i])) return true;
  }
  return false;
}

UserFunction *stackify_nonescaping_heap_allocs(UserFunction *uf) {
  FunctionBuilder builder = {0};
  builder.block_terminated = true;

  SlotEscapeStat *escape_stat = calloc(sizeof(SlotEscapeStat), uf->slots);

  for (int i = 0; i < uf->body.blocks_len; ++i) {
    Instr *instr_cur = BLOCK_START(uf, i), *instr_end = BLOCK_END(uf, i);
    while (instr_cur != instr_end) {
      if (instr_cur->type == INSTR_ALLOC_OBJECT) {
        AllocObjectInstr *instr = (AllocObjectInstr*) instr_cur;
        escape_stat[slot_index_rt(uf, instr->parent_slot)].test_escapes = true;
        ia_append(&escape_stat[slot_index_rt(uf, instr->parent_slot)], slot_index_rt(uf, instr->target_slot));
        ia_append(&escape_stat[slot_index_rt(uf, instr->parent_slot)], slot_index_rt(uf, instr->target_slot));
      } else if (instr_cur->type == INSTR_ALLOC_STATIC_OBJECT) {
        AllocStaticObjectInstr *instr = (AllocStaticObjectInstr*) instr_cur;
        escape_stat[slot_index_rt(uf, instr->parent_slot)].test_escapes = true;
        ia_append(&escape_stat[slot_index_rt(uf, instr->parent_slot)], slot_index_rt(uf, instr->target_slot));
        for (int k = 0; k < instr->tbl.entries_stored; ++k) {
          int slot = slot_index_rt(uf, ASOI_INFO(instr)[k].slot);
          // TODO track through refslots
          escape_stat[slot].test_escapes = true;
          ia_append(&escape_stat[slot], slot_index_rt(uf, instr->target_slot));
        }
      } else if (instr_cur->type == INSTR_MOVE) {
        MoveInstr *instr = (MoveInstr*) instr_cur;
        if (instr->source.kind == ARG_SLOT) {
          escape_stat[slot_index_rt(uf, instr->source.slot)].definitely_escapes = true;
        }
      } else if (instr_cur->type == INSTR_ASSIGN) {
        AssignInstr *instr = (AssignInstr*) instr_cur;
        if (instr->value.kind == ARG_SLOT) {
          escape_stat[slot_index_rt(uf, instr->value.slot)].definitely_escapes = true;
        }
      } else if (instr_cur->type == INSTR_ASSIGN_STRING_KEY) {
        AssignStringKeyInstr *instr = (AssignStringKeyInstr*) instr_cur;
        if (instr->value.kind == ARG_SLOT) {
          escape_stat[slot_index_rt(uf, instr->value.slot)].definitely_escapes = true;
        }
      } else if (instr_cur->type == INSTR_RETURN) {
        ReturnInstr *instr = (ReturnInstr*) instr_cur;
        if (instr->ret.kind == ARG_SLOT) escape_stat[slot_index_rt(uf, instr->ret.slot)].definitely_escapes = true;
      } else if (instr_cur->type == INSTR_CALL) {
        CallInstr *instr = (CallInstr*) instr_cur;
        if (instr->info.this_arg.kind == ARG_SLOT) {
          escape_stat[slot_index_rt(uf, instr->info.this_arg.slot)].definitely_escapes = true;
        }
        for (int k = 0; k < instr->info.args_len; k++) {
          Arg arg = ((Arg*)(&instr->info + 1))[k];
          if (arg.kind == ARG_SLOT) {
            escape_stat[slot_index_rt(uf, arg.slot)].definitely_escapes = true;
          }
        }
      } else if (instr_cur->type == INSTR_ALLOC_CLOSURE_OBJECT) {
        AllocClosureObjectInstr *instr = (AllocClosureObjectInstr*) instr_cur;
        escape_stat[slot_index_rt(uf, instr->context_slot)].test_escapes = true;
        if (instr->target.kind == ARG_SLOT) {
          ia_append(&escape_stat[slot_index_rt(uf, instr->context_slot)], slot_index_rt(uf, instr->target.slot));
        }
      }
      instr_cur = (Instr*) ((char*) instr_cur + instr_size(instr_cur));
    }
  }

  for (int i = 0; i < uf->body.blocks_len; ++i) {
    new_block(&builder);

    Instr *instr_cur = BLOCK_START(uf, i), *instr_end = BLOCK_END(uf, i);
    while (instr_cur != instr_end) {
      if (instr_cur->type == INSTR_ALLOC_OBJECT) {
        AllocObjectInstr *instr = (AllocObjectInstr*) instr_cur;
        AllocObjectInstr aloi = *instr;
        if (!any_escape(escape_stat, slot_index_rt(uf, aloi.target_slot))) {
          aloi.alloc_stack = true;
        }
        addinstr_like(&builder, &uf->body, instr_cur, sizeof(aloi), (Instr*) &aloi);
      } else if (instr_cur->type == INSTR_ALLOC_STATIC_OBJECT) {
        AllocStaticObjectInstr *instr = (AllocStaticObjectInstr*) instr_cur;
        int instrsz = instr_size(instr_cur);
        AllocStaticObjectInstr *asoi = alloca(instrsz);
        memcpy(asoi, instr, instrsz);
        if (!any_escape(escape_stat, slot_index_rt(uf, asoi->target_slot))) {
          asoi->alloc_stack = true;
        }
        addinstr_like(&builder, &uf->body, instr_cur, instrsz, (Instr*) asoi);
      } else {
        addinstr_like(&builder, &uf->body, instr_cur, instr_size(instr_cur), instr_cur);
      }
      instr_cur = (Instr*) ((char*) instr_cur + instr_size(instr_cur));
    }
  }

  for (int i = 0; i < uf->slots; i++) free(escape_stat[i].ptr);
  free(escape_stat);

  UserFunction *fn = build_function(&builder);
  copy_fn_stats(uf, fn);
  free_function(uf);
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
      KeyInObjInstr *kioi = (KeyInObjInstr*) instr;
      SetConstraintInstr *sci = (SetConstraintInstr*) instr;
      if (instr->type == INSTR_ALLOC_STRING_OBJECT
        && asoi->target.kind == ARG_SLOT
        && prim_slot[slot_index(asoi->target.slot)] == true)
      {
        if (slot_table_len < slot_index(asoi->target.slot) + 1) {
          slot_table_ptr = realloc(slot_table_ptr, sizeof(char*) * (slot_index(asoi->target.slot) + 1));
          for (int i = slot_table_len; i < slot_index(asoi->target.slot) + 1; ++i) {
            slot_table_ptr[i] = NULL;
          }
          slot_table_len = slot_index(asoi->target.slot) + 1;
        }
        slot_table_ptr[slot_index(asoi->target.slot)] = asoi->value;
        instr = (Instr*)(asoi + 1);
        continue; // no need to add, we're fully inlining this
      }
      if (instr->type == INSTR_ACCESS
        && acci->key.kind == ARG_SLOT
        && slot_index(acci->key.slot) < slot_table_len && slot_table_ptr[slot_index(acci->key.slot)] != NULL)
      {
        char *key_ptr = slot_table_ptr[slot_index(acci->key.slot)];
        FastKey key = prepare_key(key_ptr, strlen(key_ptr));
        AccessStringKeyInstr aski = {
          .base = { .type = INSTR_ACCESS_STRING_KEY },
          .obj = acci->obj,
          .key_slot = acci->key.slot,
          .target = acci->target,
          .key = key
        };
        addinstr_like(&builder, &uf->body, instr, sizeof(aski), (Instr*) &aski);
        instr = (Instr*)(acci + 1);
        continue;
      }
      if (instr->type == INSTR_KEY_IN_OBJ
        && kioi->key.kind == ARG_SLOT
        && slot_index(kioi->key.slot) < slot_table_len && slot_table_ptr[slot_index(kioi->key.slot)] != NULL)
      {
        StringKeyInObjInstr skioi = {
          .base = { .type = INSTR_STRING_KEY_IN_OBJ },
          .obj = kioi->obj,
          .target = kioi->target,
          .key = prepare_key(slot_table_ptr[slot_index(kioi->key.slot)], strlen(slot_table_ptr[slot_index(kioi->key.slot)]))
        };
        addinstr_like(&builder, &uf->body, instr, sizeof(skioi), (Instr*) &skioi);
        instr = (Instr*)(kioi + 1);
        continue;
      }
      if (instr->type == INSTR_ASSIGN
        && assi->key.kind == ARG_SLOT
        && slot_index(assi->key.slot) < slot_table_len && slot_table_ptr[slot_index(assi->key.slot)] != NULL)
      {
        int key_idx = slot_index(assi->key.slot);
        AssignStringKeyInstr aski = {
          .base = { .type = INSTR_ASSIGN_STRING_KEY },
          .obj = assi->obj,
          .value = assi->value,
          .key = prepare_key(slot_table_ptr[key_idx], strlen(slot_table_ptr[key_idx])),
          .type = assi->type,
          .target_slot = assi->target_slot,
        };
        addinstr_like(&builder, &uf->body, instr, sizeof(aski), (Instr*) &aski);
        instr = (Instr*)(assi + 1);
        continue;
      }
      if (instr->type == INSTR_SET_CONSTRAINT && sci->key.kind == ARG_SLOT
        && slot_index(sci->key.slot) < slot_table_len && slot_table_ptr[slot_index(sci->key.slot)] != NULL
      ) {
        char *key_ptr = slot_table_ptr[slot_index(sci->key.slot)];
        FastKey key = prepare_key(key_ptr, strlen(key_ptr));
        SetConstraintStringKeyInstr scski = {
          .base = { .type = INSTR_SET_CONSTRAINT_STRING_KEY },
          .obj = sci->obj,
          .constraint = sci->constraint,
          .key = key
        };
        addinstr_like(&builder, &uf->body, instr, sizeof(scski), (Instr*) &scski);
        instr = (Instr*)(sci + 1);
        continue;
      }
      addinstr_like(&builder, &uf->body, instr, instr_size(instr), instr);

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
Value lookup_statically(Object *obj, FastKey key, bool *key_found_p) {
  *key_found_p = false;
  while (obj) {
    TableEntry *entry = table_lookup_prepared(&obj->tbl, &key);
    if (entry) {
      // hit, but the value might change later! bad!
      if (!(obj->flags & OBJ_FROZEN)) {
        // printf("hit for %.*s, but object wasn't frozen\n", (int) key.len, key.ptr);
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
      // printf("hit for %.*s, but object wasn't closed\n", (int) key.len, key.ptr);
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
          CASE(INSTR_ALLOC_OBJECT, AllocObjectInstr)
            slot_live[slot_index_rt(uf, instr->parent_slot)] = true;
          CASE(INSTR_ALLOC_INT_OBJECT, AllocIntObjectInstr)
            if (instr->target.kind == ARG_SLOT) slot_live[slot_index_rt(uf, instr->target.slot)] = true;
          CASE(INSTR_ALLOC_BOOL_OBJECT, AllocBoolObjectInstr)
            if (instr->target.kind == ARG_SLOT) slot_live[slot_index_rt(uf, instr->target.slot)] = true;
          CASE(INSTR_ALLOC_FLOAT_OBJECT, AllocFloatObjectInstr)
            if (instr->target.kind == ARG_SLOT) slot_live[slot_index_rt(uf, instr->target.slot)] = true;
          CASE(INSTR_ALLOC_ARRAY_OBJECT, AllocArrayObjectInstr)
            if (instr->target.kind == ARG_SLOT) slot_live[slot_index_rt(uf, instr->target.slot)] = true;
          CASE(INSTR_ALLOC_STRING_OBJECT, AllocStringObjectInstr)
            if (instr->target.kind == ARG_SLOT) slot_live[slot_index_rt(uf, instr->target.slot)] = true;
          CASE(INSTR_ALLOC_CLOSURE_OBJECT, AllocClosureObjectInstr)
            if (instr->target.kind == ARG_SLOT) slot_live[slot_index_rt(uf, instr->target.slot)] = true;
            slot_live[slot_index_rt(uf, instr->context_slot)] = true;
          CASE(INSTR_CLOSE_OBJECT, CloseObjectInstr)
            slot_live[slot_index_rt(uf, instr->slot)] = true;
          CASE(INSTR_FREEZE_OBJECT, FreezeObjectInstr)
            slot_live[slot_index_rt(uf, instr->slot)] = true;
          CASE(INSTR_ACCESS, AccessInstr)
            if (instr->obj.kind == ARG_SLOT) slot_live[slot_index_rt(uf, instr->obj.slot)] = true;
            if (instr->key.kind == ARG_SLOT) slot_live[slot_index_rt(uf, instr->key.slot)] = true;
            if (instr->target.kind == ARG_SLOT) slot_live[slot_index_rt(uf, instr->target.slot)] = true;
          CASE(INSTR_ASSIGN, AssignInstr)
            if (instr->obj.kind == ARG_SLOT) slot_live[slot_index_rt(uf, instr->obj.slot)] = true;
            if (instr->key.kind == ARG_SLOT) slot_live[slot_index_rt(uf, instr->key.slot)] = true;
            if (instr->value.kind == ARG_SLOT) slot_live[slot_index_rt(uf, instr->value.slot)] = true;
          CASE(INSTR_KEY_IN_OBJ, KeyInObjInstr)
            if (instr->key.kind == ARG_SLOT) slot_live[slot_index_rt(uf, instr->key.slot)] = true;
            if (instr->obj.kind == ARG_SLOT) slot_live[slot_index_rt(uf, instr->obj.slot)] = true;
            if (instr->target.kind == ARG_SLOT) slot_live[slot_index_rt(uf, instr->target.slot)] = true;
          CASE(INSTR_IDENTICAL, IdenticalInstr)
            if (instr->obj1.kind == ARG_SLOT) slot_live[slot_index_rt(uf, instr->obj1.slot)] = true;
            if (instr->obj2.kind == ARG_SLOT) slot_live[slot_index_rt(uf, instr->obj2.slot)] = true;
            if (instr->target.kind == ARG_SLOT) slot_live[slot_index_rt(uf, instr->target.slot)] = true;
          CASE(INSTR_INSTANCEOF, InstanceofInstr)
            if (instr->obj.kind == ARG_SLOT) slot_live[slot_index_rt(uf, instr->obj.slot)] = true;
            if (instr->proto.kind == ARG_SLOT) slot_live[slot_index_rt(uf, instr->proto.slot)] = true;
          CASE(INSTR_SET_CONSTRAINT, SetConstraintInstr)
            if (instr->obj.kind == ARG_SLOT) slot_live[slot_index_rt(uf, instr->obj.slot)] = true;
            if (instr->key.kind == ARG_SLOT) slot_live[slot_index_rt(uf, instr->key.slot)] = true;
            if (instr->constraint.kind == ARG_SLOT) slot_live[slot_index_rt(uf, instr->constraint.slot)] = true;
          CASE(INSTR_TEST, TestInstr)
            if (instr->value.kind == ARG_SLOT) slot_live[slot_index_rt(uf, instr->value.slot)] = true;
          CASE(INSTR_CALL, CallInstr)
            if (instr->info.fn.kind == ARG_SLOT) slot_live[slot_index_rt(uf, instr->info.fn.slot)] = true;
            if (instr->info.this_arg.kind == ARG_SLOT) slot_live[slot_index_rt(uf, instr->info.this_arg.slot)] = true;
            for (int k = 0; k < instr->info.args_len; ++k) {
              Arg arg = ((Arg*)(&instr->info + 1))[k];
              if (arg.kind == ARG_SLOT) slot_live[slot_index_rt(uf, arg.slot)] = true;
            }
          CASE(INSTR_RETURN, ReturnInstr)
            if (instr->ret.kind == ARG_SLOT) slot_live[slot_index_rt(uf, instr->ret.slot)] = true;
          CASE(INSTR_BR, BranchInstr)
          CASE(INSTR_TESTBR, TestBranchInstr)
            if (instr->test.kind == ARG_SLOT) slot_live[slot_index_rt(uf, instr->test.slot)] = true;
          CASE(INSTR_PHI, PhiInstr)
            if (instr->arg1.kind == ARG_SLOT) slot_live[slot_index_rt(uf, instr->arg1.slot)] = true;
            if (instr->arg2.kind == ARG_SLOT) slot_live[slot_index_rt(uf, instr->arg2.slot)] = true;
          CASE(INSTR_ACCESS_STRING_KEY, AccessStringKeyInstr)
            if (instr->obj.kind == ARG_SLOT) slot_live[slot_index_rt(uf, instr->obj.slot)] = true;
            if (instr->target.kind == ARG_SLOT) slot_live[slot_index_rt(uf, instr->target.slot)] = true;
          CASE(INSTR_ASSIGN_STRING_KEY, AssignStringKeyInstr)
            if (instr->obj.kind == ARG_SLOT) slot_live[slot_index_rt(uf, instr->obj.slot)] = true;
            if (instr->value.kind == ARG_SLOT) slot_live[slot_index_rt(uf, instr->value.slot)] = true;
          CASE(INSTR_STRING_KEY_IN_OBJ, StringKeyInObjInstr)
            if (instr->obj.kind == ARG_SLOT) slot_live[slot_index_rt(uf, instr->obj.slot)] = true;
            if (instr->target.kind == ARG_SLOT) slot_live[slot_index_rt(uf, instr->target.slot)] = true;
          CASE(INSTR_SET_CONSTRAINT_STRING_KEY, SetConstraintStringKeyInstr)
            if (instr->obj.kind == ARG_SLOT) slot_live[slot_index_rt(uf, instr->obj.slot)] = true;
            if (instr->constraint.kind == ARG_SLOT) slot_live[slot_index_rt(uf, instr->constraint.slot)] = true;
          CASE(INSTR_MOVE, MoveInstr)
            if (instr->source.kind == ARG_SLOT) slot_live[slot_index_rt(uf, instr->source.slot)] = true;
          CASE(INSTR_DEFINE_REFSLOT, DefineRefslotInstr)
            slot_live[slot_index_rt(uf, instr->obj_slot)] = true;
          CASE(INSTR_ALLOC_STATIC_OBJECT, AllocStaticObjectInstr)
            for (int k = 0; k < instr->tbl.entries_stored; ++k)
              slot_live[slot_index_rt(uf, ASOI_INFO(instr)[k].slot)] = true;
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
        if (mi->target.kind == ARG_SLOT && !slot_live[slot_index_rt(uf, mi->target.slot)]) {
          add = false;
        }
      }
      if (add) addinstr_like(&builder, &uf->body, instr, instr_size(instr), instr);

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
            FunctionObject *fn_obj = (FunctionObject*) fn_obj_n;
            int size = sizeof(CallFunctionDirectInstr) + sizeof(Arg) * instr->size;
            CallFunctionDirectInstr *cfdi = alloca(size);
            cfdi->base = (Instr) {
              .type = INSTR_CALL_FUNCTION_DIRECT,
            };
            cfdi->size = size;
            if (fn_obj->dispatch_fn_ptr) {
              cfdi->fast = true;
              cfdi->dispatch_fn = fn_obj->dispatch_fn_ptr;
            } else {
              cfdi->fast = false;
              cfdi->fn = fn_obj->fn_ptr;
            }
            cfdi->info = instr->info;
            for (int i = 0; i < instr->info.args_len; ++i) {
              ((Arg*)(&cfdi->info + 1))[i] = ((Arg*)(&instr->info + 1))[i];
            }
            addinstr_like(&builder, &uf->body, instr_cur, size, (Instr*) cfdi);
            instr_cur = (Instr*) ((char*) instr + instr->size);
            continue;
          }
        }
      }

      addinstr_like(&builder, &uf->body, instr_cur, instr_size(instr_cur), instr_cur);
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
          SlotIsStaticObjInfo *rec = &static_info[slot_index_rt(uf, aski->obj.slot)];
          if (rec->static_object) {
            int field = static_info_find_field(rec, aski->key.key);
            assert(field != -1);
            slot_constraints[slot_index_rt(uf, aski->target.slot)] = &rec->constraints_ptr[field];
          }
        }
      }
      if (instr->type == INSTR_DEFINE_REFSLOT) {
        DefineRefslotInstr *dri = (DefineRefslotInstr*) instr;
        SlotIsStaticObjInfo *rec = &static_info[slot_index_rt(uf, dri->obj_slot)];
        if (rec->static_object) {
          int field = static_info_find_field(rec, dri->key.key);
          if (field != -1) {
            refslot_constraints[refslot_index_rt(uf, dri->target_refslot)] = &rec->constraints_ptr[field];
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
          object_known[slot_index_rt(uf, mi->target.slot)] = true;
          known_values_table[slot_index_rt(uf, mi->target.slot)] = mi->source.value;
        }
      }

      AccessStringKeyInstr *aski = (AccessStringKeyInstr*) instr;
      if (instr->type == INSTR_ACCESS_STRING_KEY && aski->target.kind == ARG_SLOT) {
        if (aski->obj.kind == ARG_SLOT && aski->target.kind == ARG_SLOT) {
          if (object_known[slot_index_rt(uf, aski->obj.slot)]) {
            Value known_val = known_values_table[slot_index_rt(uf, aski->obj.slot)];
            bool key_found;
            Value static_lookup = lookup_statically(closest_obj(state, known_val),
                                                      aski->key,
                                                      &key_found);
            if (key_found) {
              object_known[slot_index_rt(uf, aski->target.slot)] = true;
              known_values_table[slot_index_rt(uf, aski->target.slot)] = static_lookup;
            }
          }
        }

        ConstraintInfo *constraints = NULL;
        if (aski->obj.kind == ARG_SLOT && slot_constraints[slot_index_rt(uf, aski->obj.slot)]) {
          constraints = slot_constraints[slot_index_rt(uf, aski->obj.slot)];
        }
        if (aski->obj.kind == ARG_REFSLOT && refslot_constraints[refslot_index_rt(uf, aski->obj.refslot)]) {
          constraints = refslot_constraints[refslot_index_rt(uf, aski->obj.refslot)];
        }
        if (constraints) {
          int num_dominant = 0;
          // fprintf(stderr, "typed object go! %i constraints\n", constraints->constraint_len);
          for (int k = 0; k < constraints->constraint_len; ++k) {
            Object *constraint = constraints->constraint_ptr[k];
            Instr *location = constraints->constraint_imposed_here_ptr[k];
            if (constraint == int_base || constraint == float_base) { // primitives, always closed
              if (dominates(uf, node2rpost, sfidoms_ptr, location, instr)) {
                object_known[slot_index_rt(uf, aski->target.slot)] = true;
                bool key_found = false;
                known_values_table[slot_index_rt(uf, aski->target.slot)] = object_lookup_p(constraint, &aski->key, &key_found);
                if (!key_found) {
                  // That's alright - it'll just error at runtime.
                  object_known[slot_index_rt(uf, aski->target.slot)] = false;
                  // fprintf(stderr, "wat? static lookup on primitive-constrained field to %.*s not found in constraint object??\n", (int) aski->key.len, aski->key.ptr);
                  // abort();
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
        if (aski->target.kind == ARG_SLOT && object_known[slot_index_rt(uf, aski->target.slot)]) {
          replace_with_mv = true;
          // Note: there's no need to gc-pin this, since it's
          // clearly a value that we can see anyways
          // (ie. it's covered via the gc link via context)
          val = known_values_table[slot_index_rt(uf, aski->target.slot)];
          opt_info = my_asprintf("inlined lookup to '%s'", aski->key.key);
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
        addinstr_like(&builder, &uf->body, instr, sizeof(mi), (Instr*) &mi);
      } else {
        addinstr_like(&builder, &uf->body, instr, instr_size(instr), instr);
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
#define CHKSLOT_READ(SLOT) num_slot_use[slot_index_rt(uf, SLOT)]++;
#define CHKSLOT_WRITE(SLOT) num_slot_use[slot_index_rt(uf, SLOT)]++;
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
#undef CHKSLOT_READ
#undef CHKSLOT_WRITE
      instr_cur = (Instr*) ((char*) instr_cur + instr_size(instr_cur));
    }
  }

  for (int i = 0; i < uf->body.blocks_len; ++i) {
    new_block(&builder);

    Instr *instr_cur = BLOCK_START(uf, i), *instr_end = BLOCK_END(uf, i);
    while (instr_cur != instr_end) {
      if (instr_cur->type == INSTR_CALL) {
        CallInstr *instr = (CallInstr*) instr_cur;
        if (instr->info.target.kind == ARG_SLOT && num_slot_use[slot_index_rt(uf, instr->info.target.slot)] == 2) {
          int size = sizeof(CallInstr) + sizeof(Arg) * instr->info.args_len;
          Instr *instr_next = (Instr*) ((char*) instr_cur + size);
          MoveInstr *mi_next = (MoveInstr*) instr_next;
          if (instr_next->type == INSTR_MOVE
            && mi_next->source.kind == ARG_SLOT && slot_index_rt(uf, mi_next->source.slot) == slot_index_rt(uf, instr->info.target.slot)
          ) {
            CallInstr *ci = alloca(size);
            *ci = *instr;
            for (int k = 0; k < instr->info.args_len; ++k) {
              ((Arg*)(&ci->info + 1))[k] = ((Arg*)(&instr->info + 1))[k];
            }
            ci->info.target = mi_next->target;
            addinstr_like(&builder, &uf->body, instr_cur, size, (Instr*)ci);
            instr_cur = (Instr*) (mi_next + 1);
            continue;
          }
        }
      }

      if (instr_cur->type == INSTR_ACCESS_STRING_KEY) {
        AccessStringKeyInstr *instr = (AccessStringKeyInstr*) instr_cur;
        if (instr->target.kind == ARG_SLOT && num_slot_use[slot_index_rt(uf, instr->target.slot)] == 2) {
          Instr *instr_next = (Instr*) ((char*) instr_cur + sizeof(AccessStringKeyInstr));
          MoveInstr *mi_next = (MoveInstr*) instr_next;
          if (instr_next->type == INSTR_MOVE
            && mi_next->source.kind == ARG_SLOT && slot_index_rt(uf, mi_next->source.slot) == slot_index_rt(uf, instr->target.slot)
          ) {
            AccessStringKeyInstr aski = *instr;
            aski.target = mi_next->target;
            addinstr_like(&builder, &uf->body, instr_cur, sizeof(aski), (Instr*)&aski);
            instr_cur = (Instr*) (mi_next + 1);
            continue;
          }
        }
      }

      int sz = instr_size(instr_cur);
      addinstr_like(&builder, &uf->body, instr_cur, sz, (Instr*) instr_cur);
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
  Refslot *refslots = find_refslots(uf);

  for (int i = 0; i < uf->body.blocks_len; ++i) {
    new_block(&builder);

    Instr *instr_cur = BLOCK_START(uf, i), *instr_end = BLOCK_END(uf, i);
    while (instr_cur != instr_end) {
      if (instr_cur->type == INSTR_CALL) {
        CallInstr *instr = (CallInstr*) instr_cur;
        Arg fn = instr->info.fn;
        if (fn.kind == ARG_SLOT) {
          if (NOT_NULL(constant_slots[slot_index_rt(uf, fn.slot)])) {
            fn = (Arg) { .kind = ARG_VALUE, .value = constant_slots[slot_index_rt(uf, fn.slot)] };
          } else if (refslots[slot_index_rt(uf, fn.slot)].index != -1) {
            fn = (Arg) { .kind = ARG_REFSLOT, .refslot = refslots[slot_index_rt(uf, fn.slot)] };
          }
        }
        Arg this_arg = instr->info.this_arg;
        if (this_arg.kind == ARG_SLOT) {
          if (NOT_NULL(constant_slots[slot_index_rt(uf, this_arg.slot)])) {
            this_arg = (Arg) { .kind = ARG_VALUE, .value = constant_slots[slot_index_rt(uf, this_arg.slot)] };
          } else if (refslots[slot_index_rt(uf, this_arg.slot)].index != -1) {
            this_arg = (Arg) { .kind = ARG_REFSLOT, .refslot = refslots[slot_index_rt(uf, this_arg.slot)] };
          }
        }
        WriteArg target = instr->info.target;
        if (target.kind == ARG_SLOT) {
          if (NOT_NULL(constant_slots[slot_index_rt(uf, target.slot)])) {
            fprintf(stderr, "bad bytecode - call and store in .. value??\n");
            abort();
          } else if (refslots[slot_index_rt(uf, target.slot)].index != -1) {
            abort(); // TODO
            target = (WriteArg) { .kind = ARG_REFSLOT, .refslot = refslots[slot_index_rt(uf, target.slot)] };
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
            if (NOT_NULL(constant_slots[slot_index_rt(uf, arg.slot)])) {
              arg = (Arg) { .kind = ARG_VALUE, .value = constant_slots[slot_index_rt(uf, arg.slot)] };
            } else if (refslots[slot_index_rt(uf, arg.slot)].index != -1) {
              arg = (Arg) { .kind = ARG_REFSLOT, .refslot = refslots[slot_index_rt(uf, arg.slot)] };
            }
          }
          ((Arg*)(&ci->info + 1))[k] = arg;
        }
        addinstr_like(&builder, &uf->body, instr_cur, size, (Instr*)ci);
        instr_cur = (Instr*) ((char*) instr_cur + instr_size(instr_cur));
        continue;
      }

      if (instr_cur->type == INSTR_KEY_IN_OBJ) {
        KeyInObjInstr instr = *(KeyInObjInstr*) instr_cur;
        if (instr.key.kind == ARG_SLOT) {
          if (NOT_NULL(constant_slots[slot_index_rt(uf, instr.key.slot)])) {
            instr.key = (Arg) { .kind = ARG_VALUE, .value = constant_slots[slot_index_rt(uf, instr.key.slot)] };
          } else if (refslots[slot_index_rt(uf, instr.key.slot)].index != -1) {
            instr.key = (Arg) { .kind = ARG_REFSLOT, .refslot = refslots[slot_index_rt(uf, instr.key.slot)] };
          }
        }
        if (instr.obj.kind == ARG_SLOT) {
          if (NOT_NULL(constant_slots[slot_index_rt(uf, instr.obj.slot)])) {
            instr.obj = (Arg) { .kind = ARG_VALUE, .value = constant_slots[slot_index_rt(uf, instr.obj.slot)] };
          } else if (refslots[slot_index_rt(uf, instr.obj.slot)].index != -1) {
            instr.obj = (Arg) { .kind = ARG_REFSLOT, .refslot = refslots[slot_index_rt(uf, instr.obj.slot)] };
          }
        }
        if (instr.target.kind == ARG_SLOT) {
          if (NOT_NULL(constant_slots[slot_index_rt(uf, instr.target.slot)])) {
            fprintf(stderr, "bad bytecode - access key and store in .. value??\n");
            abort();
          } else if (refslots[slot_index_rt(uf, instr.target.slot)].index != -1) {
            abort(); // TODO
            instr.target = (WriteArg) { .kind = ARG_REFSLOT, .refslot = refslots[slot_index_rt(uf, instr.target.slot)] };
          }
        }
        addinstr_like(&builder, &uf->body, instr_cur, sizeof(instr), (Instr*) &instr);
        instr_cur = (Instr*) ((char*) instr_cur + sizeof(instr));
        continue;
      }

      if (instr_cur->type == INSTR_ACCESS_STRING_KEY) {
        AccessStringKeyInstr instr = *(AccessStringKeyInstr*) instr_cur;
        if (instr.obj.kind == ARG_SLOT) {
          if (NOT_NULL(constant_slots[slot_index_rt(uf, instr.obj.slot)])) {
            instr.obj = (Arg) { .kind = ARG_VALUE, .value = constant_slots[slot_index_rt(uf, instr.obj.slot)] };
          } else if (refslots[slot_index_rt(uf, instr.obj.slot)].index != -1) {
            instr.obj = (Arg) { .kind = ARG_REFSLOT, .refslot = refslots[slot_index_rt(uf, instr.obj.slot)] };
          }
        }
        if (instr.target.kind == ARG_SLOT) {
          if (NOT_NULL(constant_slots[slot_index_rt(uf, instr.target.slot)])) {
            fprintf(stderr, "bad bytecode - access key and store in .. value??\n");
            abort();
          } else if (refslots[slot_index_rt(uf, instr.target.slot)].index != -1) {
            abort(); // TODO
            instr.target = (WriteArg) { .kind = ARG_REFSLOT, .refslot = refslots[slot_index_rt(uf, instr.target.slot)] };
          }
        }
        addinstr_like(&builder, &uf->body, instr_cur, sizeof(instr), (Instr*) &instr);
        instr_cur = (Instr*) ((char*) instr_cur + sizeof(instr));
        continue;
      }

      if (instr_cur->type == INSTR_MOVE) {
        MoveInstr instr = *(MoveInstr*) instr_cur;
        if (instr.source.kind == ARG_SLOT) {
          if (NOT_NULL(constant_slots[slot_index_rt(uf, instr.source.slot)])) {
            instr.source = (Arg) { .kind = ARG_VALUE, .value = constant_slots[slot_index_rt(uf, instr.source.slot)] };
          } else if (refslots[slot_index_rt(uf, instr.source.slot)].index != -1) {
            instr.source = (Arg) { .kind = ARG_REFSLOT, .refslot = refslots[slot_index_rt(uf, instr.source.slot)] };
          }
        }
        addinstr_like(&builder, &uf->body, instr_cur, sizeof(instr), (Instr*) &instr);
        instr_cur = (Instr*) ((char*) instr_cur + sizeof(instr));
        continue;
      }

      if (instr_cur->type == INSTR_TESTBR) {
        TestBranchInstr instr = *(TestBranchInstr*) instr_cur;
        if (instr.test.kind == ARG_SLOT) {
          if (NOT_NULL(constant_slots[slot_index_rt(uf, instr.test.slot)])) {
            instr.test = (Arg) { .kind = ARG_VALUE, .value = constant_slots[slot_index_rt(uf, instr.test.slot)] };
          } /*else if (refslots[instr.test.slot] != -1) { // invalid state - branch instructions mustn't access refslots
            instr.test = (Arg) { .kind = ARG_REFSLOT, .refslot = refslots[instr.test.slot] };
          }*/
        }
        addinstr_like(&builder, &uf->body, instr_cur, sizeof(instr), (Instr*) &instr);
        instr_cur = (Instr*) ((char*) instr_cur + sizeof(instr));
        continue;
      }

      if (instr_cur->type == INSTR_PHI) {
        PhiInstr instr = *(PhiInstr*) instr_cur;
        if (instr.target.kind == ARG_SLOT) {
          if (NOT_NULL(constant_slots[slot_index_rt(uf, instr.target.slot)])) {
            fprintf(stderr, "bad bytecode - compute phi node and store in .. value??\n");
            abort();
          } else if (refslots[slot_index_rt(uf, instr.target.slot)].index != -1) {
            abort(); // TODO
            instr.target = (WriteArg) { .kind = ARG_REFSLOT, .refslot = refslots[slot_index_rt(uf, instr.target.slot)] };
          }
        }
        if (instr.arg1.kind == ARG_SLOT) {
          if (NOT_NULL(constant_slots[slot_index_rt(uf, instr.arg1.slot)])) {
            instr.arg1 = (Arg) { .kind = ARG_VALUE, .value = constant_slots[slot_index_rt(uf, instr.arg1.slot)] };
          } else if (refslots[slot_index_rt(uf, instr.arg1.slot)].index != -1) {
            instr.arg1 = (Arg) { .kind = ARG_REFSLOT, .refslot = refslots[slot_index_rt(uf, instr.arg1.slot)] };
          }
        }
        if (instr.arg2.kind == ARG_SLOT) {
          if (NOT_NULL(constant_slots[slot_index_rt(uf, instr.arg2.slot)])) {
            instr.arg2 = (Arg) { .kind = ARG_VALUE, .value = constant_slots[slot_index_rt(uf, instr.arg2.slot)] };
          } else if (refslots[slot_index_rt(uf, instr.arg2.slot)].index != -1) {
            instr.arg2 = (Arg) { .kind = ARG_REFSLOT, .refslot = refslots[slot_index_rt(uf, instr.arg2.slot)] };
          }
        }
        addinstr_like(&builder, &uf->body, instr_cur, sizeof(instr), (Instr*) &instr);
        instr_cur = (Instr*) ((char*) instr_cur + sizeof(instr));
        continue;
      }

      if (instr_cur->type == INSTR_RETURN) {
        ReturnInstr instr = *(ReturnInstr*) instr_cur;
        if (instr.ret.kind == ARG_SLOT) {
          if (NOT_NULL(constant_slots[slot_index_rt(uf, instr.ret.slot)])) {
            instr.ret = (Arg) { .kind = ARG_VALUE, .value = constant_slots[slot_index_rt(uf, instr.ret.slot)] };
          } /*else if (refslots[instr.ret.slot] != -1) { // invalid state - branch instructions mustn't access refslots
            instr.ret = (Arg) { .kind = ARG_REFSLOT, .refslot = refslots[instr.ret.slot] };
          }*/
        }
        addinstr_like(&builder, &uf->body, instr_cur, sizeof(instr), (Instr*) &instr);
        instr_cur = (Instr*) ((char*) instr_cur + sizeof(instr));
        continue;
      }

      if (instr_cur->type == INSTR_SET_CONSTRAINT_STRING_KEY) {
        SetConstraintStringKeyInstr instr = *(SetConstraintStringKeyInstr*) instr_cur;
        if (instr.obj.kind == ARG_SLOT) {
          if (NOT_NULL(constant_slots[slot_index_rt(uf, instr.obj.slot)])) {
            instr.obj = (Arg) { .kind = ARG_VALUE, .value = constant_slots[slot_index_rt(uf, instr.obj.slot)] };
          } else if (refslots[slot_index_rt(uf, instr.obj.slot)].index != -1) {
            instr.obj = (Arg) { .kind = ARG_REFSLOT, .refslot = refslots[slot_index_rt(uf, instr.obj.slot)] };
          }
        }
        if (instr.constraint.kind == ARG_SLOT) {
          if (NOT_NULL(constant_slots[slot_index_rt(uf, instr.constraint.slot)])) {
            instr.constraint = (Arg) { .kind = ARG_VALUE, .value = constant_slots[slot_index_rt(uf, instr.constraint.slot)] };
          } else if (refslots[slot_index_rt(uf, instr.constraint.slot)].index != -1) {
            instr.constraint = (Arg) { .kind = ARG_REFSLOT, .refslot = refslots[slot_index_rt(uf, instr.constraint.slot)] };
          }
        }
        addinstr_like(&builder, &uf->body, instr_cur, sizeof(instr), (Instr*) &instr);
        instr_cur = (Instr*) ((char*) instr_cur + sizeof(instr));
        continue;
      }

      if (instr_cur->type == INSTR_ACCESS) {
        AccessInstr instr = *(AccessInstr*) instr_cur;
        if (instr.obj.kind == ARG_SLOT) {
          if (NOT_NULL(constant_slots[slot_index_rt(uf, instr.obj.slot)])) {
            instr.obj = (Arg) { .kind = ARG_VALUE, .value = constant_slots[slot_index_rt(uf, instr.obj.slot)] };
          } else if (refslots[slot_index_rt(uf, instr.obj.slot)].index != -1) {
            instr.obj = (Arg) { .kind = ARG_REFSLOT, .refslot = refslots[slot_index_rt(uf, instr.obj.slot)] };
          }
        }
        if (instr.target.kind == ARG_SLOT) {
          if (NOT_NULL(constant_slots[slot_index_rt(uf, instr.target.slot)])) {
            fprintf(stderr, "bad bytecode - access key and store in .. value??\n");
            abort();
          } else if (refslots[slot_index_rt(uf, instr.target.slot)].index != -1) {
            abort(); // TODO
            instr.target = (WriteArg) { .kind = ARG_REFSLOT, .refslot = refslots[slot_index_rt(uf, instr.target.slot)] };
          }
        }
        if (instr.key.kind == ARG_SLOT) {
          if (NOT_NULL(constant_slots[slot_index_rt(uf, instr.key.slot)])) {
            instr.key = (Arg) { .kind = ARG_VALUE, .value = constant_slots[slot_index_rt(uf, instr.key.slot)] };
          } else if (refslots[slot_index_rt(uf, instr.key.slot)].index != -1) {
            instr.key = (Arg) { .kind = ARG_REFSLOT, .refslot = refslots[slot_index_rt(uf, instr.key.slot)] };
          }
        }
        addinstr_like(&builder, &uf->body, instr_cur, sizeof(instr), (Instr*) &instr);
        instr_cur = (Instr*) ((char*) instr_cur + sizeof(instr));
        continue;
      }

      if (instr_cur->type == INSTR_INSTANCEOF) {
        InstanceofInstr instr = *(InstanceofInstr*) instr_cur;
        if (instr.obj.kind == ARG_SLOT) {
          if (NOT_NULL(constant_slots[slot_index_rt(uf, instr.obj.slot)])) {
            instr.obj = (Arg) { .kind = ARG_VALUE, .value = constant_slots[slot_index_rt(uf, instr.obj.slot)] };
          } else if (refslots[slot_index_rt(uf, instr.obj.slot)].index != -1) {
            instr.obj = (Arg) { .kind = ARG_REFSLOT, .refslot = refslots[slot_index_rt(uf, instr.obj.slot)] };
          }
        }
        if (instr.proto.kind == ARG_SLOT) {
          if (NOT_NULL(constant_slots[slot_index_rt(uf, instr.proto.slot)])) {
            instr.proto = (Arg) { .kind = ARG_VALUE, .value = constant_slots[slot_index_rt(uf, instr.proto.slot)] };
          } else if (refslots[slot_index_rt(uf, instr.proto.slot)].index != -1) {
            instr.proto = (Arg) { .kind = ARG_REFSLOT, .refslot = refslots[slot_index_rt(uf, instr.proto.slot)] };
          }
        }
        if (instr.target.kind == ARG_SLOT) {
          if (NOT_NULL(constant_slots[slot_index_rt(uf, instr.target.slot)])) {
            fprintf(stderr, "bad bytecode - instanceof and store in .. value??\n");
            abort();
          } else if (refslots[slot_index_rt(uf, instr.target.slot)].index != -1) {
            abort(); // TODO
            instr.target = (WriteArg) { .kind = ARG_REFSLOT, .refslot = refslots[slot_index_rt(uf, instr.target.slot)] };
          }
        }
        addinstr_like(&builder, &uf->body, instr_cur, sizeof(instr), (Instr*) &instr);
        instr_cur = (Instr*) ((char*) instr_cur + sizeof(instr));
        continue;
      }

      if (instr_cur->type == INSTR_ASSIGN) {
        AssignInstr instr = *(AssignInstr*) instr_cur;
        if (instr.obj.kind == ARG_SLOT) {
          if (NOT_NULL(constant_slots[slot_index_rt(uf, instr.obj.slot)])) {
            instr.obj = (Arg) { .kind = ARG_VALUE, .value = constant_slots[slot_index_rt(uf, instr.obj.slot)] };
          } else if (refslots[slot_index_rt(uf, instr.obj.slot)].index != -1) {
            instr.obj = (Arg) { .kind = ARG_REFSLOT, .refslot = refslots[slot_index_rt(uf, instr.obj.slot)] };
          }
        }
        if (instr.value.kind == ARG_SLOT) {
          if (NOT_NULL(constant_slots[slot_index_rt(uf, instr.value.slot)])) {
            instr.value = (Arg) { .kind = ARG_VALUE, .value = constant_slots[slot_index_rt(uf, instr.value.slot)] };
          } else if (refslots[slot_index_rt(uf, instr.value.slot)].index != -1) {
            instr.value = (Arg) { .kind = ARG_REFSLOT, .refslot = refslots[slot_index_rt(uf, instr.value.slot)] };
          }
        }
        if (instr.key.kind == ARG_SLOT) {
          if (NOT_NULL(constant_slots[slot_index_rt(uf, instr.key.slot)])) {
            instr.key = (Arg) { .kind = ARG_VALUE, .value = constant_slots[slot_index_rt(uf, instr.key.slot)] };
          } else if (refslots[slot_index_rt(uf, instr.key.slot)].index != -1) {
            instr.key = (Arg) { .kind = ARG_REFSLOT, .refslot = refslots[slot_index_rt(uf, instr.key.slot)] };
          }
        }
        addinstr_like(&builder, &uf->body, instr_cur, sizeof(instr), (Instr*) &instr);
        instr_cur = (Instr*) ((char*) instr_cur + sizeof(instr));
        continue;
      }

      if (instr_cur->type == INSTR_ASSIGN_STRING_KEY) {
        AssignStringKeyInstr instr = *(AssignStringKeyInstr*) instr_cur;
        if (instr.obj.kind == ARG_SLOT) {
          if (NOT_NULL(constant_slots[slot_index_rt(uf, instr.obj.slot)])) {
            instr.obj = (Arg) { .kind = ARG_VALUE, .value = constant_slots[slot_index_rt(uf, instr.obj.slot)] };
          } else if (refslots[slot_index_rt(uf, instr.obj.slot)].index != -1) {
            instr.obj = (Arg) { .kind = ARG_REFSLOT, .refslot = refslots[slot_index_rt(uf, instr.obj.slot)] };
          }
        }
        if (instr.value.kind == ARG_SLOT) {
          if (NOT_NULL(constant_slots[slot_index_rt(uf, instr.value.slot)])) {
            instr.value = (Arg) { .kind = ARG_VALUE, .value = constant_slots[slot_index_rt(uf, instr.value.slot)] };
          } else if (refslots[slot_index_rt(uf, instr.value.slot)].index != -1) {
            instr.value = (Arg) { .kind = ARG_REFSLOT, .refslot = refslots[slot_index_rt(uf, instr.value.slot)] };
          }
        }
        addinstr_like(&builder, &uf->body, instr_cur, sizeof(instr), (Instr*) &instr);
        instr_cur = (Instr*) ((char*) instr_cur + sizeof(instr));
        continue;
      }

      addinstr_like(&builder, &uf->body, instr_cur, instr_size(instr_cur), instr_cur);
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
            if (slot_index_rt(uf, aski->obj.slot) != slot_index_rt(uf, alobi->target_slot)) { failed = true; break; }

            info_ptr = realloc(info_ptr, sizeof(StaticFieldInfo) * ++info_len);
            info_ptr[info_len - 1] = (StaticFieldInfo) {0};
            StaticFieldInfo *info = &info_ptr[info_len - 1];
            info->key = aski->key;
            info->slot = aski->value.slot;
            info->refslot = (Refslot) { .index = -1 };

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
              if (info->key.hash == dri->key.hash) {
                if (info->refslot.index != -1) {
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

            if (slot_index_rt(uf, scski->obj.slot) != slot_index_rt(uf, alobi->target_slot)) break;
            if (scski->constraint.kind != ARG_SLOT && scski->constraint.kind != ARG_VALUE) break;
            if (scski->constraint.kind == ARG_SLOT && !IS_OBJ(constant_slots[slot_index_rt(uf, scski->constraint.slot)])) break; // wat wat

            for (int k = 0; k < info_len; ++k) {
              StaticFieldInfo *info = &info_ptr[k];
              if (info->key.hash == scski->key.hash) {
                if (info->constraint) abort(); // wat wat wat
                if (scski->constraint.kind == ARG_SLOT) info->constraint = AS_OBJ(constant_slots[slot_index_rt(uf, scski->constraint.slot)]);
                else info->constraint = AS_OBJ(scski->constraint.value);
              }
            }

            instr_reading = (Instr*) (scski + 1);
          }
          Object sample_obj = {0};
          for (int k = 0; k < info_len; ++k) {
            StaticFieldInfo *info = &info_ptr[k];
            FastKey key = info->key;
            char *error = object_set(state, &sample_obj, &key, VNULL);
            if (error) { fprintf(stderr, "INTERNAL LOGIC ERROR: %s\n", error); abort(); }
          }
          for (int k = 0; k < info_len; ++k) {
            StaticFieldInfo *info = &info_ptr[k];
            FastKey key = info->key;
            TableEntry *entry = table_lookup_prepared(&sample_obj.tbl, &key);
            if (!entry) { fprintf(stderr, "where has it gone?? missing %s\n", info->key.key); abort(); }
            info->offset = (char*) entry - (char*) sample_obj.tbl.entries_ptr;
          }

          AllocStaticObjectInstr *asoi = alloca(sizeof(AllocStaticObjectInstr)
                                                + sizeof(StaticFieldInfo) * info_len);
          *asoi = (AllocStaticObjectInstr) {
            .base = { .type = INSTR_ALLOC_STATIC_OBJECT },
            .tbl = sample_obj.tbl,
            .parent_slot = alobi->parent_slot,
            .target_slot = alobi->target_slot,
          };

          int info_idx = 0;
          for (int k = 0; k < sample_obj.tbl.entries_num; k++) {
            TableEntry *entry = &sample_obj.tbl.entries_ptr[k];
            if (entry->hash) {
              StaticFieldInfo *info_entry = NULL;
              for (int l = 0; l < info_len; ++l) {
                if (info_ptr[l].key.hash == entry->hash) {
                  info_entry = &info_ptr[l];
                  break;
                }
              }
              assert(info_entry);
              ASOI_INFO(asoi)[info_idx++] = *info_entry;
            }
          }
          assert(info_idx == info_len);

          addinstr_like(&builder, &uf->body, instr, instr_size((Instr*) asoi), (Instr*) asoi);
          instr = instr_reading;
          continue;
        }
      }

      addinstr_like(&builder, &uf->body, instr, instr_size(instr), instr);
      instr = (Instr*) ((char*) instr + instr_size(instr));
    }
  }
  UserFunction *fn = build_function(&builder);
  copy_fn_stats(uf, fn);
  free_function(uf);
  return fn;
}

// TODO make something fast, lol
typedef struct _SortedUniqList SortedUniqList;
struct _SortedUniqList {
  int value;
  SortedUniqList *next;
};

void sl_print(FILE *file, SortedUniqList *list) {
  fprintf(file, "{");
  bool first = true;
  while (list) {
    if (first) { first = false; } else { fprintf(file, ", "); }
    fprintf(file, "%i", list->value);
    list = list->next;
  }
  fprintf(file, "}");
}

bool sl_add_to(SortedUniqList **target, SortedUniqList *source) {
  bool changed = false;
  SortedUniqList **cursor = target;
  while (source) {
    while (*cursor && (*cursor)->value < source->value) cursor = &(*cursor)->next;
    // now cursor->value >= source->value, insert source
    if (*cursor && (*cursor)->value == source->value) { } // already in target
    else {
      SortedUniqList *next = *cursor;
      *cursor = malloc(sizeof(SortedUniqList));
      (*cursor)->value = source->value;
      (*cursor)->next = next;
      changed = true;
    }
    source = source->next;
  }
  return changed;
}

bool sl_add_value_to(SortedUniqList **target, int value) {
  SortedUniqList entry = { .value = value };
  return sl_add_to(target, &entry);
}

bool sl_subtract_from(SortedUniqList **target, SortedUniqList *source) {
  bool changed = false;
  SortedUniqList **cursor = target;
  while (source) {
    while (*cursor && (*cursor)->value < source->value) cursor = &(*cursor)->next;
    // now cursor->value >= source->value, insert source
    if (*cursor && (*cursor)->value == source->value) {
      SortedUniqList *old_ptr = *cursor;
      *cursor = (*cursor)->next;
      changed = true;
      free(old_ptr);
    }
    source = source->next;
  }
  return changed;
}

bool sl_subtract_value(SortedUniqList **target, int value) {
  SortedUniqList entry = { .value = value };
  return sl_subtract_from(target, &entry);
}

bool sl_contains(SortedUniqList *slist, int value) {
  while (slist) {
    if (slist->value > value) return false;
    if (slist->value == value) return true;
    slist = slist->next;
  }
  return false;
}

void sl_free(SortedUniqList *slist) {
  while (slist) {
    SortedUniqList *next = slist->next;
    free(slist);
    slist = next;
  }
}

static void reassign_slot(UserFunction *uf, Slot *slot_p, bool read, int special_slots, bool last_access_blk, bool *slot_inuse, int *slot_map, SortedUniqList *slot_outlist) {
  int slot = slot_index_rt(uf, *slot_p);
  if (read) {
    *slot_p = (Slot) { .index = slot_map[slot] };
    assert(uf->resolved);
    resolve_slot_ref(uf, slot_p);
    if (slot >= special_slots && !sl_contains(slot_outlist, slot) && last_access_blk) {
      slot_inuse[slot_map[slot]] = false;
      // fprintf(stderr, "open slot %i -> %i for access\n", slot, slot_map[slot]);
    }
    return;
  }

  assert(!read);
  int selected_slot = -1;
  for (int k = 0; k < uf->slots; ++k) if (!slot_inuse[k]) {
    selected_slot = k;
    break;
  }
  assert(selected_slot != -1);
  slot_inuse[selected_slot] = true;
  // fprintf(stderr, "map: %i => %i\n", slot, selected_slot);
  slot_map[slot] = selected_slot;

  *slot_p = (Slot) { .index = slot_map[slot] };
  assert(uf->resolved);
  resolve_slot_ref(uf, slot_p);
}

// gather all slots that we read that we don't write
bool update_block_inlist(UserFunction *uf, int block, SortedUniqList **target, SortedUniqList *out_list) {
  SortedUniqList *read_list = NULL;
  SortedUniqList *write_list = NULL;

  Instr *instr_cur = BLOCK_START(uf, block), *instr_end = BLOCK_END(uf, block);
  while (instr_cur != instr_end) {
    switch (instr_cur->type) {
#define CASE(KEY, TY) } break; case KEY: { TY *instr = (TY*) instr_cur; (void) instr;
#define CHKSLOT_READ(S) sl_add_value_to(&read_list, slot_index_rt(uf, S));
#define CHKSLOT_WRITE(S) sl_add_value_to(&write_list, slot_index_rt(uf, S));
        case INSTR_INVALID: { abort();
#include "vm/slots.txt"
          CASE(INSTR_LAST, Instr) abort();
        } break;
        default: assert("Unhandled Instruction Type!" && false);

#undef CHKSLOT_READ
#undef CHKSLOT_WRITE
#undef CASE
    }
    instr_cur = (Instr*) ((char*) instr_cur + instr_size(instr_cur));
  }
  SortedUniqList *in_list = read_list;
  sl_add_to(&in_list, out_list); // will be read later
  sl_subtract_from(&in_list, write_list);
  sl_free(write_list);
  bool res = sl_add_to(target, in_list);
  sl_free(in_list);
  return res;
}

void determine_slot_liveness(UserFunction *uf, SortedUniqList ***slot_inlist, SortedUniqList ***slot_outlist) {
  // fprintf(stderr, "for:\n");
  // dump_fn(NULL, uf);
  CFG cfg;
  cfg_build(&cfg, uf);
  // perform flow analysis; see https://en.wikipedia.org/wiki/Live_variable_analysis
  SortedUniqList *workqueue = NULL;
  for (int i = 0; i < cfg.nodes_len; i++) {
    if (cfg.nodes_ptr[i].succ_len == 0) {
      sl_add_value_to(&workqueue, i);
    }
  }
  *slot_inlist = calloc(uf->slots, sizeof(SortedUniqList*));
  *slot_outlist = calloc(uf->slots, sizeof(SortedUniqList*));
  while (workqueue) {
    int blk = workqueue->value;
    sl_subtract_value(&workqueue, blk);

    bool changed_outlist = false;
    // outlist = for succ: union(inlist[succ])
    for (int i = 0; i < cfg.nodes_ptr[blk].succ_len; i++) {
      changed_outlist |= sl_add_to(&(*slot_outlist)[blk], (*slot_inlist)[cfg.nodes_ptr[blk].succ_ptr[i]]);
    }
    if (changed_outlist) {
      // fprintf(stderr, "%i updated outlist to ", blk);
      // sl_print(stderr, (*slot_outlist)[blk]);
      // fprintf(stderr, "\n");
    }

    bool changed_inlist = update_block_inlist(uf, blk, &(*slot_inlist)[blk], (*slot_outlist)[blk]);
    if (changed_inlist) {
      // fprintf(stderr, "%i updated inlist to ", blk);
      // sl_print(stderr, (*slot_inlist)[blk]);
      // fprintf(stderr, "\n");

      // update our preds
      for (int i = 0; i < cfg.nodes_ptr[blk].pred_len; i++) {
        sl_add_value_to(&workqueue, cfg.nodes_ptr[blk].pred_ptr[i]);
      }
    }
  }
}

UserFunction *free_stack_objects_early(UserFunction *uf) {
  FunctionBuilder builder = {0};
  builder.block_terminated = true;

  // Note: frees for stackframes are placed after the point of last use, even if that
  // use is a subframe allocation. THIS IS SAFE, surprisingly enough, since they'll
  // only _actually_ be freed once the allocs above them are clear.

  // Note: refslot reads keep stackframes alive.
  // Obviously.

  SortedUniqList *stack_allocated_obj = NULL;

  SortedUniqList **slot_inlist, **slot_outlist;
  determine_slot_liveness(uf, &slot_inlist, &slot_outlist);

  Instr **blk_last_access = malloc(sizeof(Instr*) * uf->slots);
  Slot *dying_object_slots = calloc(sizeof(Slot), uf->slots);

  for (int blk = 0; blk < uf->body.blocks_len; ++blk) {
    new_block(&builder);

    // precomp first/last access per instr inside the block, for slots that die in the block
    {
      bzero(blk_last_access, sizeof(Instr*) * uf->slots);
      Instr *instr_cur = BLOCK_START(uf, blk), *instr_end = BLOCK_END(uf, blk);
      while (instr_cur != instr_end) {
        if (instr_cur->type == INSTR_ALLOC_OBJECT) {
          AllocObjectInstr *instr = (AllocObjectInstr*) instr_cur;
          if (instr->alloc_stack) {
            sl_add_value_to(&stack_allocated_obj, slot_index_rt(uf, instr->target_slot));
          }
        }
        if (instr_cur->type == INSTR_ALLOC_STATIC_OBJECT) {
          AllocStaticObjectInstr *instr = (AllocStaticObjectInstr*) instr_cur;
          if (instr->alloc_stack) {
            sl_add_value_to(&stack_allocated_obj, slot_index_rt(uf, instr->target_slot));
          }
        }
        switch (instr_cur->type) {
#define CASE(KEY, TY) } break; case KEY: { TY *instr = (TY*) instr_cur; (void) instr;
#define CHKSLOT_READ(S) blk_last_access[slot_index_rt(uf, S)] = instr_cur;
#define CHKSLOT_WRITE(S) blk_last_access[slot_index_rt(uf, S)] = instr_cur;
          case INSTR_INVALID: { abort(); Instr *instr = NULL; (void) instr;
#include "vm/slots.txt"
            CASE(INSTR_LAST, Instr) abort();
          } break;
          default: assert("Unhandled Instruction Type!" && false);
#undef CHKSLOT_READ
#undef CHKSLOT_WRITE
#undef CASE
        }
        instr_cur = (Instr*) ((char*) instr_cur + instr_size(instr_cur));
      }
    }

    int num_dying_slots = 0;
    for (int k = 0; k < uf->slots; k++) {
      if (blk_last_access[k] && !sl_contains(slot_outlist[blk], k) && sl_contains(stack_allocated_obj, k)) {
        Slot slot = (Slot) { .index = k };
        assert(uf->resolved);
        resolve_slot_ref(uf, &slot);
        dying_object_slots[num_dying_slots++] = slot;
      }
    }

    Instr *instr_cur = BLOCK_START(uf, blk), *instr_end = BLOCK_END(uf, blk);
    while (instr_cur != instr_end) {
      if (instr_cur->type == INSTR_ALLOC_STATIC_OBJECT)
      {
        AllocStaticObjectInstr *asoi = (AllocStaticObjectInstr*) instr_cur;
        int instrSlot = slot_index_rt(uf, asoi->target_slot);
        bool deadObject = false;
        for (int k = 0; k < num_dying_slots; k++) {
          int slot = slot_index_rt(uf, dying_object_slots[k]);
          if (slot == instrSlot) {
            deadObject = instr_cur == blk_last_access[slot];
            break;
          }
        }
        if (deadObject) { } // unused object; remove
        else {
          addinstr_like(&builder, &uf->body, instr_cur, instr_size(instr_cur), instr_cur);
        }
      } else {
        // Instr *instr2 = instr_cur; dump_instr(NULL, &instr2);
        addinstr_like(&builder, &uf->body, instr_cur, instr_size(instr_cur), instr_cur);

        for (int k = 0; k < num_dying_slots; k++) {
          int slot = slot_index_rt(uf, dying_object_slots[k]);
          if (instr_cur == blk_last_access[slot]) {
            // fprintf(stderr, "and generate free for %i\n", slot);
            FreeObjectInstr instr = {
              .base = { .type = INSTR_FREE_OBJECT },
              .on_stack = true,
              .obj_slot = (Slot) { .index = slot }
            };
            assert(uf->resolved);
            resolve_slot_ref(uf, &instr.obj_slot);
            // TODO assign to debug info of allocation
            // instead of some rando instr that's the last to use it
            addinstr_like(&builder, &uf->body, instr_cur, sizeof(instr), (Instr*) &instr);
          }
        }
      }

      instr_cur = (Instr*) ((char*) instr_cur + instr_size(instr_cur));
    }
    bzero(dying_object_slots, sizeof(int) * num_dying_slots);
  }

  free(blk_last_access);
  free(dying_object_slots);
  for (int i = 0; i < uf->slots; i++) {
    sl_free(slot_inlist[i]);
    sl_free(slot_outlist[i]);
  }
  free(slot_inlist);
  free(slot_outlist);

  UserFunction *fn = build_function(&builder);
  copy_fn_stats(uf, fn);
  free_function(uf);
  return fn;
}

/**
 * Given a stack object that:
 * - is assigned to a slot that is not used, only ever freed,
 *   - ie. no fields are read or written via string
 * - and never has any of its refslots written to
 * then we may safely replace its refslot reads
 * with the slot reads it was constructed from!
 */
UserFunction *deconstruct_immutable_stack_objects(UserFunction *uf) {
  FunctionBuilder builder = {0};
  builder.block_terminated = true;

  SortedUniqList *mutable_stack_objects = NULL;
  SortedUniqList *nonfreed_read_slots = NULL;

  for (int i = 0; i < uf->body.blocks_len; ++i) {
    Instr *instr_cur = BLOCK_START(uf, i), *instr_end = BLOCK_END(uf, i);
#define CHKSLOT_REF_WRITE(SLOT) \
    sl_add_value_to(&mutable_stack_objects, slot_index_rt(uf, find_refslot_slot(uf, SLOT)));
#define CHKSLOT_READ_RW(SLOT) \
    if (instr_cur->type != INSTR_FREE_OBJECT) { sl_add_value_to(&nonfreed_read_slots, slot_index_rt(uf, SLOT)); }
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
#undef CHKSLOT_READ_RW
#undef CHKSLOT_REF_WRITE
      instr_cur = (Instr*) ((char*) instr_cur + instr_size(instr_cur));
    }
  }

  bool *redirect_refslot = calloc(sizeof(bool), uf->refslots);
  Slot *field_for_refslot = calloc(sizeof(Slot), uf->refslots);

  for (int blk = 0; blk < uf->body.blocks_len; ++blk) {
    new_block(&builder);

    Instr *instr_cur = BLOCK_START(uf, blk), *instr_end = BLOCK_END(uf, blk);
    while (instr_cur != instr_end) {
      int instrsz = instr_size(instr_cur);
      if (instr_cur->type == INSTR_ALLOC_STATIC_OBJECT) {
        addinstr_like(&builder, &uf->body, instr_cur, instrsz, instr_cur);

        AllocStaticObjectInstr *asoi = (AllocStaticObjectInstr*) instr_cur;
        int target_slot = slot_index_rt(uf, asoi->target_slot);
        if (!sl_contains(mutable_stack_objects, target_slot) && !sl_contains(nonfreed_read_slots, target_slot)) {
          for (int k = 0; k < asoi->tbl.entries_stored; ++k) {
            int refslot = refslot_index_rt(uf, ASOI_INFO(instr_cur)[k].refslot);
            redirect_refslot[refslot] = true;
            field_for_refslot[refslot] = ASOI_INFO(instr_cur)[k].slot;
          }
        }
      } else {
        Instr *instr_new = alloca(instrsz);
        memcpy(instr_new, instr_cur, instrsz);
#define READ_SLOT(SLOT) \
        if ((SLOT).kind == ARG_REFSLOT && redirect_refslot[refslot_index_rt(uf, (SLOT).refslot)]) { \
          SLOT = (Arg) { .kind = ARG_SLOT, .slot = field_for_refslot[refslot_index_rt(uf, (SLOT).refslot)] }; \
        }
#define CASE(KEY, TY) } break; case KEY: { TY *instr = (TY*) instr_new; (void) instr;
        switch (instr_new->type) {
          case INSTR_INVALID: { abort();
#include "vm/slots.txt"
            CASE(INSTR_LAST, Instr) abort();
          } break;
          default: assert("Unhandled Instruction Type!" && false);
        }
#undef CASE
#undef READ_SLOT
        addinstr_like(&builder, &uf->body, instr_cur, instrsz, instr_new);
      }
      instr_cur = (Instr*) ((char*) instr_cur + instr_size(instr_cur));
    }
  }
  free(field_for_refslot);
  free(redirect_refslot);
  sl_free(mutable_stack_objects);
  sl_free(nonfreed_read_slots);

  UserFunction *fn = build_function(&builder);
  copy_fn_stats(uf, fn);
  free_function(uf);
  return fn;
}

void fixup_refslots(UserFunction *uf, int delta) {
  assert(uf->resolved);
  for (int i = 0; i < uf->body.blocks_len; ++i) {
#define CHKSLOT_REF(SLOT) SLOT.offset += delta;
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
#undef CHKSLOT_REF
      instr_cur = (Instr*) ((char*) instr_cur + instr_size(instr_cur));
    }
  }
}

// WARNING
// this function makes the IR **NON-SSA**
// and thus it **MUST** come completely last!!
UserFunction *compactify_registers(UserFunction *uf) {
  FunctionBuilder builder = {0};
  builder.block_terminated = true;

  SortedUniqList **slot_inlist, **slot_outlist;
  determine_slot_liveness(uf, &slot_inlist, &slot_outlist);

  bool *slot_inuse = calloc(sizeof(bool), uf->slots);
  int *slot_map = calloc(sizeof(int), uf->slots);
  Instr **blk_last_access = calloc(sizeof(Instr*), uf->slots);

  // specials
  int special_slots = 2 + uf->arity;
  for (int i = 0; i < special_slots; ++i) {
    slot_inuse[i] = true;
    slot_map[i] = i;
  }

  for (int i = 0; i < uf->body.blocks_len; ++i) {
    new_block(&builder);

    memset(slot_inuse + special_slots, 0, sizeof(bool) * (uf->slots - special_slots));
    for (int k = special_slots; k < uf->slots; k++) {
      if (sl_contains(slot_inlist[i], k)) slot_inuse[slot_map[k]] = true;
    }
    /*
    fprintf(stderr, "updating block %i:\n", i);
    for (int k = special_slots; k < uf->slots; k++) {
      if (slot_inuse[k]) fprintf(stderr, "| %i in use.\n", k);
    }
    */
    // precomp first/last access per instr inside the block, for slots that don't escape the block
    {
      Instr *instr_cur = BLOCK_START(uf, i), *instr_end = BLOCK_END(uf, i);
      while (instr_cur != instr_end) {
        switch (instr_cur->type) {
#define CASE(KEY, TY) } break; case KEY: { TY *instr = (TY*) instr_cur; (void) instr;
#define CHKSLOT_READ(S) blk_last_access[slot_index_rt(uf, S)] = instr_cur;
#define CHKSLOT_WRITE(S) blk_last_access[slot_index_rt(uf, S)] = instr_cur;
          case INSTR_INVALID: { abort(); Instr *instr = NULL; (void) instr;
#include "vm/slots.txt"
            CASE(INSTR_LAST, Instr) abort();
          } break;
          default: assert("Unhandled Instruction Type!" && false);
#undef CHKSLOT_READ
#undef CHKSLOT_WRITE
#undef CASE
        }
        instr_cur = (Instr*) ((char*) instr_cur + instr_size(instr_cur));
      }
    }

    Instr *instr_cur = BLOCK_START(uf, i), *instr_end = BLOCK_END(uf, i);
    while (instr_cur != instr_end) {
      switch (instr_cur->type) {
#define CASE(KEY, TY) \
          use_range_start(&builder, *instr_belongs_to_p(&uf->body, instr_cur));\
          addinstr(&builder, sz, (Instr*) instr);\
          use_range_end(&builder, *instr_belongs_to_p(&uf->body, instr_cur));\
          instr_cur = (Instr*) ((char*) instr_cur + sz);\
          continue;\
        }\
        case KEY: {\
          int sz = instr_size(instr_cur);\
          TY *instr = (TY*) alloca(sz);\
          memcpy(instr, instr_cur, sz);

#define CHKSLOT_READ_RW(S) reassign_slot(uf, &S, true, special_slots, instr_cur == blk_last_access[slot_index_rt(uf, S)], slot_inuse, slot_map,\
                                      slot_outlist[i])
#define CHKSLOT_WRITE_RW(S) reassign_slot(uf, &S, false, special_slots, instr_cur == blk_last_access[slot_index_rt(uf, S)], slot_inuse, slot_map,\
                                       slot_outlist[i])

        case INSTR_INVALID: { abort(); Instr *instr = NULL; int sz = 0;
#include "vm/slots.txt"
          CASE(INSTR_LAST, Instr) abort();
        } break;
        default: assert("Unhandled Instruction Type!" && false);

#undef CHKSLOT_READ
#undef CHKSLOT_WRITE
#undef CASE
      }
      instr_cur = (Instr*) ((char*) instr_cur + instr_size(instr_cur));
    }
  }

  int maxslot = 0;
  for (int i = 0; i < uf->slots; ++i) if (slot_map[i] > maxslot) maxslot = slot_map[i];
  free(blk_last_access);
  free(slot_inuse);
  free(slot_map);
  for (int i = 0; i < uf->slots; i++) {
    sl_free(slot_inlist[i]);
    sl_free(slot_outlist[i]);
  }
  free(slot_inlist);
  free(slot_outlist);

  UserFunction *fn = build_function(&builder);
  copy_fn_stats(uf, fn);
  int old_slots = uf->slots;
  free_function(uf);
  fn->slots = maxslot + 1;
  fn->non_ssa = true;

  fixup_refslots(fn, (fn->slots - old_slots) * sizeof(Value));
  // dump_fn(NULL, fn);
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
        BranchInstr bri = *instr;
        bri.blk = blk_map[bri.blk];
        addinstr_like(&builder, &uf->body, instr_cur, sizeof(bri), (Instr*) &bri);
        instr_cur = (Instr*) (instr + 1);
        continue;
      }
      if (instr_cur->type == INSTR_TESTBR) {
        TestBranchInstr *instr = (TestBranchInstr*) instr_cur;
        TestBranchInstr tbri = *instr;
        tbri.true_blk = blk_map[tbri.true_blk];
        tbri.false_blk = blk_map[tbri.false_blk];
        addinstr_like(&builder, &uf->body, instr_cur, sizeof(tbri), (Instr*) &tbri);
        instr_cur = (Instr*) (instr + 1);
        continue;
      }
      if (instr_cur->type == INSTR_PHI) {
        PhiInstr *instr = (PhiInstr*) instr_cur;
        PhiInstr phi = *instr;
        phi.block1 = blk_map[phi.block1];
        phi.block2 = blk_map[phi.block2];
        addinstr_like(&builder, &uf->body, instr_cur, sizeof(phi), (Instr*) &phi);
        instr_cur = (Instr*) (instr + 1);
        continue;
      }
      addinstr_like(&builder, &uf->body, instr_cur, instr_size(instr_cur), instr_cur);
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
  if (uf->num_optimized > 5) {
    return uf;
  }
  if (uf->num_optimized == 5) {
    // vm_print_backtrace(state);
    fprintf(stderr, "function optimized too many times.\n");
    // __asm__("int $3");
  }
  uf->num_optimized ++;

  if (uf->non_ssa != false) {
    fprintf(stderr, "called optimizer on function that is non-ssa!\n");
    abort();
  }

  assert(uf->resolved);

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

  uf = remove_pointless_blocks(uf);

  uf = null_this_in_thisless_calls(state, uf);
  uf = stackify_nonescaping_heap_allocs(uf);
  uf = deconstruct_immutable_stack_objects(uf);
  uf = free_stack_objects_early(uf);

  // should be last-ish, micro-opt that introduces a new op
  uf = call_functions_directly(state, uf);

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

  finalize(uf);
  return uf;
}

Slot find_refslot_slot(UserFunction *uf, Refslot refslot) {
  for (int i = 0; i < uf->body.blocks_len; ++i) {
    Instr *instr = BLOCK_START(uf, i), *instr_end = BLOCK_END(uf, i);
    while (instr != instr_end) {
      if (instr->type == INSTR_DEFINE_REFSLOT) {
        DefineRefslotInstr *dri = (DefineRefslotInstr*) instr;
        instr = (Instr*) (dri + 1);
        if (refslot_index_rt(uf, dri->target_refslot) == refslot_index_rt(uf, refslot)) {
          return dri->obj_slot;
        }
        continue;
      }
      if (instr->type == INSTR_ALLOC_STATIC_OBJECT) {
        AllocStaticObjectInstr *asoi = (AllocStaticObjectInstr*) instr;
        for (int i = 0; i < asoi->tbl.entries_stored; ++i) {
          StaticFieldInfo *info = &ASOI_INFO(asoi)[i];
          if (refslot_index_rt(uf, info->refslot) == refslot_index_rt(uf, refslot)) {
            return asoi->target_slot;
          }
        }
      }
      instr = (Instr*)((char*) instr + instr_size(instr));
    }
  }
  dump_fn(NULL, uf);
  assert("Refslot not found" && false);
}
