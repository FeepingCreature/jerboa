#include "vm/optimize.h"

#include "vm/builder.h"

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
        case INSTR_INVALID: { assert(false); int _stepsize = -1;
          CASE(INSTR_GET_ROOT, GetRootInstr, get_root_instr)
          CASE(INSTR_GET_CONTEXT, GetContextInstr, get_context_instr)
          CASE(INSTR_ALLOC_OBJECT, AllocObjectInstr, alloc_obj_instr)
            slots[alloc_obj_instr->parent_slot] = false;
          CASE(INSTR_ALLOC_INT_OBJECT, AllocIntObjectInstr, alloc_int_obj_instr)
          CASE(INSTR_ALLOC_FLOAT_OBJECT, AllocFloatObjectInstr, alloc_float_obj_instr)
          CASE(INSTR_ALLOC_ARRAY_OBJECT, AllocArrayObjectInstr, alloc_array_obj_instr)
          CASE(INSTR_ALLOC_STRING_OBJECT, AllocStringObjectInstr, alloc_string_obj_instr)
          CASE(INSTR_ALLOC_CLOSURE_OBJECT, AllocClosureObjectInstr, alloc_closure_obj_instr)
            slots[alloc_closure_obj_instr->context_slot] = false;
          CASE(INSTR_CLOSE_OBJECT, CloseObjectInstr, close_obj_instr)
          CASE(INSTR_ACCESS, AccessInstr, access_instr)
            slots[access_instr->obj_slot] = false;
          CASE(INSTR_ASSIGN, AssignInstr, assign_instr)
            slots[assign_instr->obj_slot] = slots[assign_instr->value_slot] = false;
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
  char **names_ptr; int names_len;
} SlotIsStaticObjInfo;

#include <stdio.h>
#include "vm/dump.h"

// static object: allocated, assigned a few keys, and closed.
static void slot_is_static_object(UserFunction *uf, SlotIsStaticObjInfo **slots_p) {
  *slots_p = calloc(sizeof(SlotIsStaticObjInfo), uf->slots);
  
  fprintf(stderr, " -------- \nslot scanning %s\n", uf->name);
  for (int i = 0; i < uf->body.blocks_len; ++i) {
    InstrBlock *block = &uf->body.blocks_ptr[i];
    Instr *instr = block->instrs_ptr;
    Instr *instr_end = block->instrs_ptr_end;
    while (instr != instr_end) {
      Instr *instr2 = instr;
      dump_instr(&instr2);
      
      if (instr->type == INSTR_ALLOC_OBJECT) {
        AllocObjectInstr *alobi = (AllocObjectInstr*) instr;
        instr = (Instr*) (alobi + 1);
        bool failed = false;
        char **names_ptr = 0; int names_len = 0;
        while (instr != instr_end && instr->type == INSTR_ASSIGN_STRING_KEY) {
          AssignStringKeyInstr *aski = (AssignStringKeyInstr*) instr;
          if (aski->type != ASSIGN_PLAIN) { failed = true; break; }
          instr = (Instr*) (aski + 1);
          names_ptr = realloc(names_ptr, sizeof(char*) * ++names_len);
          names_ptr[names_len - 1] = aski->key;
        }
        if (instr->type != INSTR_CLOSE_OBJECT) failed = true;
        if (failed) {
          free(names_ptr);
          continue;
        }
        int target_slot = alobi->target_slot;
        (*slots_p)[target_slot].static_object = true;
        (*slots_p)[target_slot].parent_slot = alobi->parent_slot;
        (*slots_p)[target_slot].names_ptr = names_ptr;
        (*slots_p)[target_slot].names_len = names_len;
        for (int i = 0; i < names_len; ++i) {
          fprintf(stderr, "  # %i: %s\n", i, names_ptr[i]);
        }
      }
      instr = (Instr*)((char*) instr + instr_size(instr));
    }
  }
}

static void copy_fn_stats(UserFunction *from, UserFunction *to) {
  to->slots = from->slots;
  to->arity = from->arity;
  to->name = from->name;
  to->is_method = from->is_method;
}

static UserFunction *redirect_predictable_lookup_misses(UserFunction *uf) {
  SlotIsStaticObjInfo *info;
  slot_is_static_object(uf, &info);
  
  FunctionBuilder *builder = calloc(sizeof(FunctionBuilder), 1);
  builder->slot_base = 0;
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
          for (int i = 0; i < info[obj_slot].names_len; ++i) {
            char *objkey = info[obj_slot].names_ptr[i];
            if (strcmp(objkey, aski_new->key) == 0) {
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
        use_range_start(builder, instr->belongs_to);
        addinstr(builder, sizeof(*aski_new), (Instr*) aski_new);
        use_range_end(builder, instr->belongs_to);
        instr = (Instr*) (aski + 1);
        continue;
      }
      
      use_range_start(builder, instr->belongs_to);
      addinstr(builder, instr_size(instr), instr);
      use_range_end(builder, instr->belongs_to);
      
      instr = (Instr*) ((char*) instr + instr_size(instr));
    }
  }
  UserFunction *fn = build_function(builder);
  copy_fn_stats(uf, fn);
  return fn;
}

static UserFunction *inline_primitive_accesses(UserFunction *uf, bool *prim_slot) {
  FunctionBuilder *builder = calloc(sizeof(FunctionBuilder), 1);
  builder->slot_base = 0;
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
        aski->key = slot_table_ptr[acci->key_slot];
        use_range_start(builder, acci->base.belongs_to);
        addinstr(builder, sizeof(*aski), (Instr*) aski);
        use_range_end(builder, acci->base.belongs_to);
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
        use_range_start(builder, assi->base.belongs_to);
        addinstr(builder, sizeof(*aski), (Instr*) aski);
        use_range_end(builder, assi->base.belongs_to);
        instr = (Instr*)(assi + 1);
        continue;
      }
      use_range_start(builder, instr->belongs_to);
      addinstr(builder, instr_size(instr), instr);
      use_range_end(builder, instr->belongs_to);
      
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
  
  return uf;
}

#include "print.h"

UserFunction *optimize_runtime(VMState *state, UserFunction *uf, Object *context) {
  fprintf(stderr, "runtime optimize %s with %p\n", uf->name, (void*) context);
  print_recursive(state, context);
  printf("\n-----\n");
  return uf;
}
