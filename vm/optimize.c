#include "optimize.h"

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
    for (int k = 0; k < block->instrs_len; ++k) {
      Instr *instr = block->instrs_ptr[k];
#define CASE(KEY, TY, VAR) } break; case KEY: { TY *VAR = (TY*) instr; (void) VAR;
      switch (instr->type) {
        case INSTR_INVALID: { assert(false);
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
        } break;
        default: assert("Unhandled Instruction Type!" && false);
      }
#undef CASE
    }
  }
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
    
    for (int k = 0; k < block->instrs_len; ++k) {
      Instr *instr = block->instrs_ptr[k];
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
        continue; // no need to add, we're fully inlining this
      }
      if (instr->type == INSTR_ACCESS
        && acci->key_slot < slot_table_len && slot_table_ptr[acci->key_slot] != NULL)
      {
        AccessStringKeyInstr *instr = malloc(sizeof(AccessStringKeyInstr));
        instr->base.type = INSTR_ACCESS_STRING_KEY;
        instr->obj_slot = acci->obj_slot;
        instr->target_slot = acci->target_slot;
        instr->key = slot_table_ptr[acci->key_slot];
        addinstr(builder, (Instr*) instr);
        continue;
      }
      if (instr->type == INSTR_ASSIGN
        && assi->key_slot < slot_table_len && slot_table_ptr[assi->key_slot] != NULL)
      {
        AssignStringKeyInstr *instr = malloc(sizeof(AssignStringKeyInstr));
        instr->base.type = INSTR_ASSIGN_STRING_KEY;
        instr->obj_slot = assi->obj_slot;
        instr->value_slot = assi->value_slot;
        instr->key = slot_table_ptr[assi->key_slot];
        instr->type = assi->type;
        addinstr(builder, (Instr*) instr);
        continue;
      }
      addinstr(builder, instr);
    }
  }
  // TODO abstract this out?
  UserFunction *fn = build_function(builder);
  fn->slots = uf->slots;
  fn->arity = uf->arity;
  fn->name = uf->name;
  fn->is_method = uf->is_method;
  return fn;
}

UserFunction *optimize(UserFunction *uf) {
  
  bool *primitive_slots;
  slot_is_primitive(uf, &primitive_slots);
  uf = inline_primitive_accesses(uf, primitive_slots);
  return uf;
}
