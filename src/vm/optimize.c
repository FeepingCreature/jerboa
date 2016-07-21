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
            slots[instr->obj_slot] = false;
          CASE(INSTR_ASSIGN, AssignInstr)
            slots[instr->obj_slot] = slots[instr->value_slot] = false;
          // TODO inline key?
          CASE(INSTR_KEY_IN_OBJ, KeyInObjInstr)
          CASE(INSTR_SET_CONSTRAINT, SetConstraintInstr)
            slots[instr->obj_slot] = slots[instr->constraint_slot] = false;
          CASE(INSTR_CALL, CallInstr)
            slots[instr->function_slot] = slots[instr->this_slot] = false;
            for (int k = 0; k < instr->args_length; ++k) {
              slots[((int*)(instr + 1))[k]] = false;
            }
          CASE(INSTR_RETURN, ReturnInstr)
            slots[instr->ret_slot] = false;
          CASE(INSTR_SAVE_RESULT, SaveResultInstr)
          CASE(INSTR_BR, BranchInstr)
          CASE(INSTR_TESTBR, TestBranchInstr)
            slots[instr->test_slot] = false;
          CASE(INSTR_PHI, PhiInstr)
            slots[instr->slot1] = slots[instr->slot2] = slots[instr->target_slot] = false;
          CASE(INSTR_LAST, Instr) abort();
        } break;
        default: assert("Unhandled Instruction Type!" && false);
      }
#undef CASE
      instr_cur = (Instr*) ((char*) instr_cur + instr_size(instr_cur));
    }
  }
}

static Object **find_constant_slots(UserFunction *uf) {
  Object **slots = calloc(sizeof(Object*), uf->slots);
  for (int i = 0; i < uf->body.blocks_len; ++i) {
    Instr *instr = BLOCK_START(uf, i), *instr_end = BLOCK_END(uf, i);
    while (instr != instr_end) {
      if (instr->type == INSTR_SET_SLOT) {
        SetSlotInstr *ssi = (SetSlotInstr*) instr;
        slots[ssi->target_slot] = ssi->value;
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
  int field = -1;
  for (int k = 0; k < rec->fields_len; ++k) {
    int field_len = strlen(rec->names_ptr[k]);
    if (field_len == name_len && strncmp(rec->names_ptr[k], name_ptr, name_len) == 0) {
      field = k;
      break;
    }
  }
  return field;
}

#include "vm/dump.h"

// static object: allocated, assigned a few keys, and closed.
static void slot_is_static_object(UserFunction *uf, SlotIsStaticObjInfo **slots_p) {
  *slots_p = calloc(sizeof(SlotIsStaticObjInfo), uf->slots);
  
  Object **constant_slots = find_constant_slots(uf);
  
  for (int i = 0; i < uf->body.blocks_len; ++i) {
    Instr *instr = BLOCK_START(uf, i), *instr_end = BLOCK_END(uf, i);
    while (instr != instr_end) {
      if (instr->type == INSTR_ALLOC_OBJECT) {
        AllocObjectInstr *alobi = (AllocObjectInstr*) instr;
        instr = (Instr*) (alobi + 1);
        bool failed = false;
        char **names_ptr = 0; int fields_len = 0;
        while (instr != instr_end) {
          if (instr->type == INSTR_ASSIGN_STRING_KEY) {
            AssignStringKeyInstr *aski = (AssignStringKeyInstr*) instr;
            if (aski->type != ASSIGN_PLAIN) { failed = true; break; }
            if (aski->obj_slot != alobi->target_slot) { failed = true; break; }
            
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
        Object *constraint = constant_slots[scski->constraint_slot];
        SlotIsStaticObjInfo *rec = &(*slots_p)[scski->obj_slot];
        
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
  builder->block_terminated = true;
  
  for (int i = 0; i < uf->body.blocks_len; ++i) {
    new_block(builder);
    
    Instr *instr = BLOCK_START(uf, i), *instr_end = BLOCK_END(uf, i);
    while (instr != instr_end) {
      if (instr->type == INSTR_ACCESS_STRING_KEY) {
        AccessStringKeyInstr *aski = (AccessStringKeyInstr*) instr;
        AccessStringKeyInstr aski_new = *aski;
        while (true) {
          int obj_slot = aski_new.obj_slot;
          if (!info[obj_slot].static_object) break;
          int field = static_info_find_field(&info[obj_slot], aski->key_len, aski->key_ptr);
          if (field != -1) break;
          // since the key was not found in obj
          // we can know statically that the lookup will
          // not succeed at runtime either
          // (since the object is closed, its keys are statically known)
          // so instead look up in the (known) parent object from the start
          aski_new.obj_slot = info[obj_slot].parent_slot;
        }
        addinstr_like(builder, instr, sizeof(aski_new), (Instr*) &aski_new);
        instr = (Instr*) (aski + 1);
        continue;
      }
      
      if (instr->type == INSTR_ASSIGN_STRING_KEY) {
        AssignStringKeyInstr *aski = (AssignStringKeyInstr*) instr;
        if (aski->type == ASSIGN_EXISTING) {
          // TODO remove all the instr mallocs
          AssignStringKeyInstr aski_new = *aski;
          while (true) {
            int obj_slot = aski_new.obj_slot;
            if (!info[obj_slot].static_object) break;
            int field = static_info_find_field(&info[obj_slot], strlen(aski->key), aski->key);
            if (field != -1) break; // key was found, we're at the right object
            aski_new.obj_slot = info[obj_slot].parent_slot;
          }
          addinstr_like(builder, instr, sizeof(aski_new), (Instr*) &aski_new);
          instr = (Instr*) (aski + 1);
          continue;
        }
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
  builder->refslot_base = uf->refslots;
  builder->block_terminated = true;
  
  // since the object accesses must dominate the object declaration,
  // the refslot accesses will also dominate the refslot declaration.
  
  for (int i = 0; i < uf->body.blocks_len; ++i) {
    new_block(builder);
    
    Instr *instr = BLOCK_START(uf, i), *instr_end = BLOCK_END(uf, i);
    while (instr != instr_end) {
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
  builder->block_terminated = true;
  
  char **slot_table_ptr = NULL;
  int slot_table_len = 0;
  
  for (int i = 0; i < uf->body.blocks_len; ++i) {
    new_block(builder);
    
    Instr *instr = BLOCK_START(uf, i), *instr_end = BLOCK_END(uf, i);
    while (instr != instr_end) {
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
        char *key_ptr = slot_table_ptr[acci->key_slot];
        int key_len = strlen(key_ptr);
        AccessStringKeyInstr aski = {
          .base = { .type = INSTR_ACCESS_STRING_KEY },
          .obj_slot = acci->obj_slot,
          .target_slot = acci->target_slot,
          .key_ptr = key_ptr,
          .key_len = key_len,
          .key_hash = hash(key_ptr, key_len)
        };
        addinstr_like(builder, instr, sizeof(aski), (Instr*) &aski);
        instr = (Instr*)(acci + 1);
        continue;
      }
      if (instr->type == INSTR_ASSIGN
        && assi->key_slot < slot_table_len && slot_table_ptr[assi->key_slot] != NULL)
      {
        AssignStringKeyInstr aski = {
          .base = { .type = INSTR_ASSIGN_STRING_KEY },
          .obj_slot = assi->obj_slot,
          .value_slot = assi->value_slot,
          .key = slot_table_ptr[assi->key_slot],
          .type = assi->type
        };
        addinstr_like(builder, instr, sizeof(aski), (Instr*) &aski);
        instr = (Instr*)(assi + 1);
        continue;
      }
      if (instr->type == INSTR_SET_CONSTRAINT
        && sci->key_slot < slot_table_len && slot_table_ptr[sci->key_slot] != NULL)
      {
        char *key_ptr = slot_table_ptr[sci->key_slot];
        SetConstraintStringKeyInstr scski = {
          .base = { .type = INSTR_SET_CONSTRAINT_STRING_KEY },
          .obj_slot = sci->obj_slot,
          .constraint_slot = sci->constraint_slot,
          .key_ptr = key_ptr,
          .key_len = strlen(key_ptr)
        };
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
    if (!(obj->flags & OBJ_PRIMITIVE)) {
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

static UserFunction *remove_dead_slot_writes(UserFunction *uf) {
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
            slot_live[instr->target_slot] = true;
          CASE(INSTR_ALLOC_BOOL_OBJECT, AllocBoolObjectInstr)
            slot_live[instr->target_slot] = true;
          CASE(INSTR_ALLOC_FLOAT_OBJECT, AllocFloatObjectInstr)
            slot_live[instr->target_slot] = true;
          CASE(INSTR_ALLOC_ARRAY_OBJECT, AllocArrayObjectInstr)
            slot_live[instr->target_slot] = true;
          CASE(INSTR_ALLOC_STRING_OBJECT, AllocStringObjectInstr)
            slot_live[instr->target_slot] = true;
          CASE(INSTR_ALLOC_CLOSURE_OBJECT, AllocClosureObjectInstr)
            slot_live[instr->target_slot] = slot_live[instr->base.context_slot] = true;
          CASE(INSTR_CLOSE_OBJECT, CloseObjectInstr)
            slot_live[instr->slot] = true;
          CASE(INSTR_FREEZE_OBJECT, FreezeObjectInstr)
            slot_live[instr->slot] = true;
          CASE(INSTR_ACCESS, AccessInstr)
            slot_live[instr->obj_slot] = slot_live[instr->key_slot]
              = slot_live[instr->target_slot] = true;
          CASE(INSTR_ASSIGN, AssignInstr)
            slot_live[instr->obj_slot] = slot_live[instr->key_slot]
              = slot_live[instr->value_slot] = true;
          // TODO inline key?
          CASE(INSTR_KEY_IN_OBJ, KeyInObjInstr)
            slot_live[instr->key_slot] = slot_live[instr->obj_slot]
              = slot_live[instr->target_slot] = true;
          CASE(INSTR_SET_CONSTRAINT, SetConstraintInstr)
            slot_live[instr->obj_slot] = slot_live[instr->constraint_slot] = true;
          CASE(INSTR_CALL, CallInstr)
            slot_live[instr->function_slot] = slot_live[instr->this_slot] = true;
            for (int k = 0; k < instr->args_length; ++k) {
              slot_live[((int*)(instr + 1))[k]] = true;
            }
          CASE(INSTR_RETURN, ReturnInstr)
            slot_live[instr->ret_slot] = true;
          CASE(INSTR_SAVE_RESULT, SaveResultInstr)
          CASE(INSTR_BR, BranchInstr)
          CASE(INSTR_TESTBR, TestBranchInstr)
            slot_live[instr->test_slot] = true;
          CASE(INSTR_PHI, PhiInstr)
            slot_live[instr->slot1] = slot_live[instr->slot2] = true;
          CASE(INSTR_ACCESS_STRING_KEY, AccessStringKeyInstr)
            slot_live[instr->obj_slot] = slot_live[instr->target_slot] = true;
          CASE(INSTR_ASSIGN_STRING_KEY, AssignStringKeyInstr)
            slot_live[instr->obj_slot] = slot_live[instr->value_slot] = true;
          CASE(INSTR_SET_CONSTRAINT_STRING_KEY, SetConstraintStringKeyInstr)
            slot_live[instr->constraint_slot] = slot_live[instr->obj_slot] = true;
          CASE(INSTR_SET_SLOT, SetSlotInstr)
          CASE(INSTR_DEFINE_REFSLOT, DefineRefslotInstr)
            slot_live[instr->obj_slot] = true;
          CASE(INSTR_READ_REFSLOT, ReadRefslotInstr)
            slot_live[instr->target_slot] = true;
          CASE(INSTR_WRITE_REFSLOT, WriteRefslotInstr)
            slot_live[instr->source_slot] = true;
          CASE(INSTR_ALLOC_STATIC_OBJECT, AllocStaticObjectInstr)
            for (int k = 0; k < instr->info_len; ++k)
              slot_live[instr->info_ptr[k].slot] = true;
          CASE(INSTR_LAST, Instr) abort();
        } break;
        default: assert("Unhandled Instruction Type!" && false);
      }
#undef CASE
    }
  }
  
  FunctionBuilder *builder = calloc(sizeof(FunctionBuilder), 1);
  builder->block_terminated = true;
  
  for (int i = 0; i < uf->body.blocks_len; ++i) {
    new_block(builder);
    
    Instr *instr = BLOCK_START(uf, i), *instr_end = BLOCK_END(uf, i);
    while (instr != instr_end) {
      bool add = true;
      if (instr->type == INSTR_SET_SLOT) {
        SetSlotInstr *ssi = (SetSlotInstr*) instr;
        if (!slot_live[ssi->target_slot]) {
          add = false;
          // fprintf(stderr, "skip set %s\n", ssi->opt_info);
        }
      }
      if (instr->type == INSTR_SAVE_RESULT) {
        SaveResultInstr *sri = (SaveResultInstr*) instr;
        if (!slot_live[sri->target_slot]) {
          add = false;
        }
      }
      if (add) addinstr_like(builder, instr, instr_size(instr), instr);
      
      instr = (Instr*) ((char*) instr + instr_size(instr));
    }
  }
  UserFunction *fn = build_function(builder);
  copy_fn_stats(uf, fn);
  return fn;
}

bool dominates(UserFunction *uf, Node2RPost node2rpost, int *sfidoms_ptr, Instr *earlier, Instr *later);

UserFunction *inline_static_lookups_to_constants(VMState *state, UserFunction *uf, Object *context) {
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
  Object **known_objects_table = calloc(sizeof(Object*), uf->slots);
  object_known[1] = true;
  known_objects_table[1] = context;
  
  ConstraintInfo **slot_constraints = calloc(sizeof(ConstraintInfo*), uf->slots);
  
  for (int i = 0; i < uf->body.blocks_len; ++i) {
    Instr *instr = BLOCK_START(uf, i), *instr_end = BLOCK_END(uf, i);
    while (instr != instr_end) {
      if (instr->type == INSTR_SET_SLOT) {
        SetSlotInstr *ssi = (SetSlotInstr*) instr;
        object_known[ssi->target_slot] = true;
        known_objects_table[ssi->target_slot] = ssi->value;
      }
      
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
        
        SlotIsStaticObjInfo *rec = &static_info[aski->obj_slot];
        if (rec->static_object) {
          int field = static_info_find_field(rec, aski->key_len, aski->key_ptr);
          assert(field != -1);
          slot_constraints[aski->target_slot] = &rec->constraints_ptr[field];
        }
        
        if (slot_constraints[aski->obj_slot]) {
          ConstraintInfo *constraints = slot_constraints[aski->obj_slot];
          int num_dominant = 0;
          // fprintf(stderr, "typed object go! %i constraints\n", constraints->constraint_len);
          for (int k = 0; k < constraints->constraint_len; ++k) {
            Object *constraint = constraints->constraint_ptr[k];
            Instr *location = constraints->constraint_imposed_here_ptr[k];
            if (constraint == int_base || constraint == float_base) { // primitives, always closed
              if (dominates(uf, node2rpost, sfidoms_ptr, location, instr)) {
                object_known[aski->target_slot] = true;
                bool key_found = false;
                known_objects_table[aski->target_slot] = object_lookup_with_hash(constraint, aski->key_ptr, aski->key_len, aski->key_hash, &key_found);
                if (!key_found) {
                  fprintf(stderr, "wat? static lookup on primitive-constrained field to %.*s not found in constraint object??\n", aski->key_len, aski->key_ptr);
                  abort();
                }
                num_dominant ++;
              }
            }
          }
          assert(num_dominant <= 1);
        }
      }
      instr = (Instr*) ((char*) instr + instr_size(instr));
    }
  }
  cfg_destroy(&cfg);
  free(rpost2node.ptr);
  free(node2rpost.ptr);
  free(sfidoms_ptr);
  
  FunctionBuilder *builder = calloc(sizeof(FunctionBuilder), 1);
  builder->block_terminated = true;
  
  for (int i = 0; i < uf->body.blocks_len; ++i) {
    new_block(builder);
    
    Instr *instr = BLOCK_START(uf, i), *instr_end = BLOCK_END(uf, i);
    while (instr != instr_end) {
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
        opt_info = my_asprintf("inlined alloc_float %f", afoi->value);
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
        SetSlotInstr ssi = {
          .base = { .type = INSTR_SET_SLOT },
          .target_slot = target_slot,
          .value = obj,
          .opt_info = opt_info
        };
        addinstr_like(builder, instr, sizeof(ssi), (Instr*) &ssi);
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

UserFunction *fuse_static_object_alloc(UserFunction *uf) {
  FunctionBuilder *builder = calloc(sizeof(FunctionBuilder), 1);
  builder->block_terminated = true;
  
  Object **constant_slots = find_constant_slots(uf);
  
  for (int i = 0; i < uf->body.blocks_len; ++i) {
    new_block(builder);
    
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
            if (aski->obj_slot != alobi->target_slot) { failed = true; break; }
            
            info_ptr = realloc(info_ptr, sizeof(StaticFieldInfo) * ++info_len);
            info_ptr[info_len - 1] = (StaticFieldInfo) {0};
            StaticFieldInfo *info = &info_ptr[info_len - 1];
            info->name_ptr = aski->key;
            info->name_len = strlen(aski->key);
            info->name_hash = hash(info->name_ptr, info->name_len);
            info->slot = aski->value_slot;
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
          while (instr_reading != instr_end && instr_reading->type == INSTR_SET_CONSTRAINT_STRING_KEY) {
            SetConstraintStringKeyInstr *scski = (SetConstraintStringKeyInstr*) instr_reading;
            
            if (scski->obj_slot != alobi->target_slot) break;
            if (!constant_slots[scski->constraint_slot]) break; // TODO emit, then keep looking for a bit
            
            for (int k = 0; k < info_len; ++k) {
              StaticFieldInfo *info = &info_ptr[k];
              if (info->name_len == scski->key_len && strncmp(info->name_ptr, scski->key_ptr, info->name_len) == 0) {
                if (info->constraint) abort(); // wat wat wat
                info->constraint = constant_slots[scski->constraint_slot];
                refslots_set ++;
              }
            }
            
            instr_reading = (Instr*) (scski + 1);
          }
          Object sample_obj = {0};
          for (int k = 0; k < info_len; ++k) {
            StaticFieldInfo *info = &info_ptr[k];
            char *error = object_set(&sample_obj, info->name_ptr, NULL);
            if (error) { fprintf(stderr, "INTERNAL LOGIC ERROR: %s\n", error); abort(); }
          }
          
          AllocStaticObjectInstr *asoi = alloca(sizeof(AllocStaticObjectInstr) + sizeof(Object));
          *asoi = (AllocStaticObjectInstr) {
            .base = { .type = INSTR_ALLOC_STATIC_OBJECT },
            .info_len = info_len,
            .info_ptr = info_ptr,
            .parent_slot = alobi->parent_slot,
            .target_slot = alobi->target_slot,
          };
          *(Object*) (asoi + 1) = sample_obj;
          addinstr_like(builder, instr, sizeof(AllocStaticObjectInstr) + sizeof(Object), (Instr*) asoi);
          instr = instr_reading;
        }
      }
      
      addinstr_like(builder, instr, instr_size(instr), instr);
      instr = (Instr*) ((char*) instr + instr_size(instr));
    }
  }
  UserFunction *fn = build_function(builder);
  copy_fn_stats(uf, fn);
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
  FunctionBuilder *builder = calloc(sizeof(FunctionBuilder), 1);
  builder->block_terminated = true;
  
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
    new_block(builder);
    
    Instr *instr_cur = BLOCK_START(uf, i), *instr_end = BLOCK_END(uf, i);
    while (instr_cur != instr_end) {
      switch (instr_cur->type) {

#define CASE(KEY, TY) \
          use_range_start(builder, instr_cur->belongs_to);\
          builder->scope = ((Instr*) instr)->context_slot;\
          addinstr(builder, sz, (Instr*) instr);\
          use_range_end(builder, instr_cur->belongs_to);\
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
  
  UserFunction *fn = build_function(builder);
  copy_fn_stats(uf, fn);
  fn->slots = maxslot + 1;
  fn->non_ssa = true;
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
  uf = inline_static_lookups_to_constants(state, uf, context);
  // run a second time, to pick up accesses on objects that just now became statically known
  uf = inline_static_lookups_to_constants(state, uf, context);
  
  uf = access_vars_via_refslots(uf);
  
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
  
  // must be late!
  uf = fuse_static_object_alloc(uf);
  uf = remove_dead_slot_writes(uf);
  
  // must be very very *very* last!
  uf = compactify_registers(uf);
  
  uf->optimized = true; // will be optimized no further
  
  if (uf->name) {
    fprintf(stderr, "runtime optimized %s to\n", uf->name);
    dump_fn(uf);
  }
  
  return uf;
}
