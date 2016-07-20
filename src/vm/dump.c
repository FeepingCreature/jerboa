#include "vm/dump.h"

#include <stdio.h>

void dump_instr(VMState *state, Instr **instr_p) {
  Instr *instr = *instr_p;
  // fprintf(stderr, "%p", (void*) instr);
  fprintf(stderr, "    ");
  fprintf(stderr, "%i ", instr->context_slot);
  switch (instr->type) {
    case INSTR_GET_ROOT:
      fprintf(stderr, "get root: %%%i\n", ((GetRootInstr*) instr)->slot);
      *instr_p = (Instr*) ((GetRootInstr*) instr + 1);
      break;
    case INSTR_ALLOC_OBJECT:
      fprintf(stderr, "alloc object: %%%i = new object(%%%i)\n",
              ((AllocObjectInstr*) instr)->target_slot, ((AllocObjectInstr*) instr)->parent_slot);
      *instr_p = (Instr*) ((AllocObjectInstr*) instr + 1);
      break;
    case INSTR_ALLOC_INT_OBJECT:
      fprintf(stderr, "alloc int object: %%%i = new int(%i)\n",
              ((AllocIntObjectInstr*) instr)->target_slot, ((AllocIntObjectInstr*) instr)->value);
      *instr_p = (Instr*) ((AllocIntObjectInstr*) instr + 1);
      break;
    case INSTR_ALLOC_FLOAT_OBJECT:
      fprintf(stderr, "alloc float object: %%%i = new float(%f)\n",
              ((AllocFloatObjectInstr*) instr)->target_slot, ((AllocFloatObjectInstr*) instr)->value);
      *instr_p = (Instr*) ((AllocFloatObjectInstr*) instr + 1);
      break;
    case INSTR_ALLOC_ARRAY_OBJECT:
      fprintf(stderr, "alloc array object: %%%i = []\n",
              ((AllocArrayObjectInstr*) instr)->target_slot);
      *instr_p = (Instr*) ((AllocArrayObjectInstr*) instr + 1);
      break;
    case INSTR_ALLOC_STRING_OBJECT:
      fprintf(stderr, "alloc string object: %%%i = new string(%s)\n",
              ((AllocStringObjectInstr*) instr)->target_slot, ((AllocStringObjectInstr*) instr)->value);
      *instr_p = (Instr*) ((AllocStringObjectInstr*) instr + 1);
      break;
    case INSTR_ALLOC_CLOSURE_OBJECT:
      fprintf(stderr, "alloc closure object: %%%i = new function(), dumped later\n",
              ((AllocClosureObjectInstr*) instr)->target_slot);
      *instr_p = (Instr*) ((AllocClosureObjectInstr*) instr + 1);
      break;
    case INSTR_CLOSE_OBJECT:
      fprintf(stderr, "close object: %%%i\n", ((CloseObjectInstr*) instr)->slot);
      *instr_p = (Instr*) ((CloseObjectInstr*) instr + 1);
      break;
    case INSTR_FREEZE_OBJECT:
      fprintf(stderr, "freeze object: %%%i\n", ((FreezeObjectInstr*) instr)->slot);
      *instr_p = (Instr*) ((FreezeObjectInstr*) instr + 1);
      break;
    case INSTR_ACCESS:
      fprintf(stderr, "access: %%%i = %%%i . %%%i\n",
              ((AccessInstr*) instr)->target_slot, ((AccessInstr*) instr)->obj_slot, ((AccessInstr*) instr)->key_slot);
      *instr_p = (Instr*) ((AccessInstr*) instr + 1);
      break;
    case INSTR_ASSIGN:
    {
      char *mode = "(plain)";
      if (((AssignInstr*) instr)->type == ASSIGN_EXISTING) mode = "(existing)";
      else if (((AssignInstr*) instr)->type == ASSIGN_SHADOWING) mode = "(shadowing)";
      fprintf(stderr, "assign%s: %%%i . %%%i = %%%i\n",
              mode, ((AssignInstr*) instr)->obj_slot, ((AssignInstr*) instr)->key_slot, ((AssignInstr*) instr)->value_slot);
      *instr_p = (Instr*) ((AssignInstr*) instr + 1);
      break;
    }
    case INSTR_KEY_IN_OBJ:
      fprintf(stderr, "key in obj: %%%i = %%%i in %%%i\n",
              ((KeyInObjInstr*) instr)->target_slot, ((KeyInObjInstr*) instr)->key_slot, ((KeyInObjInstr*) instr)->obj_slot);
      *instr_p = (Instr*) ((KeyInObjInstr*) instr + 1);
      break;
    case INSTR_SET_CONSTRAINT:
    {
      SetConstraintInstr *sci = (SetConstraintInstr*) instr;
      fprintf(stderr, "set constraint: %%%i . %%%i : %%%i\n",
              sci->obj_slot, sci->key_slot, sci->constraint_slot);
      *instr_p = (Instr*) (sci + 1);
      break;
    }
    case INSTR_CALL:
      fprintf(stderr, "call: %%%i . %%%i ( ",
              ((CallInstr*) instr)->this_slot, ((CallInstr*) instr)->function_slot);
      for (int i = 0; i < ((CallInstr*) instr)->args_length; ++i) {
        if (i) fprintf(stderr, ", ");
        fprintf(stderr, "%%%i", ((int*)((CallInstr*) instr + 1))[i]);
      }
      fprintf(stderr, " )\n");
      *instr_p = (Instr*) ((int*)((CallInstr*) instr + 1) + ((CallInstr*) instr)->args_length);
      break;
    case INSTR_RETURN:
      fprintf(stderr, "return: %%%i\n", ((ReturnInstr*) instr)->ret_slot);
      *instr_p = (Instr*) ((ReturnInstr*) instr + 1);
      break;
    case INSTR_SAVE_RESULT:
      fprintf(stderr, "save result: -> %%%i\n",
              ((SaveResultInstr*) instr)->target_slot);
      *instr_p = (Instr*) ((SaveResultInstr*) instr + 1);
      break;
    case INSTR_BR:
      fprintf(stderr, "branch: <%i>\n", ((BranchInstr*) instr)->blk);
      *instr_p = (Instr*) ((BranchInstr*) instr + 1);
      break;
    case INSTR_TESTBR:
      fprintf(stderr, "test-branch: %%%i ? <%i> : <%i>\n",
              ((TestBranchInstr*) instr)->test_slot, ((TestBranchInstr*) instr)->true_blk, ((TestBranchInstr*) instr)->false_blk);
      *instr_p = (Instr*) ((TestBranchInstr*) instr + 1);
      break;
    case INSTR_ACCESS_STRING_KEY:
      fprintf(stderr, "access: %%%i = %%%i . '%.*s' \t\t(opt: string key)\n",
              ((AccessStringKeyInstr*) instr)->target_slot, ((AccessStringKeyInstr*) instr)->obj_slot,
              ((AccessStringKeyInstr*) instr)->key_len, ((AccessStringKeyInstr*) instr)->key_ptr);
      *instr_p = (Instr*) ((AccessStringKeyInstr*) instr + 1);
      break;
    case INSTR_ASSIGN_STRING_KEY:
    {
      char *mode = "(plain)";
      if (((AssignStringKeyInstr*) instr)->type == ASSIGN_EXISTING) mode = "(existing)";
      else if (((AssignStringKeyInstr*) instr)->type == ASSIGN_SHADOWING) mode = "(shadowing)";
      fprintf(stderr, "assign%s: %%%i . '%s' = %%%i \t\t(opt: string key)\n",
              mode, ((AssignStringKeyInstr*) instr)->obj_slot, ((AssignStringKeyInstr*) instr)->key, ((AssignStringKeyInstr*) instr)->value_slot);
      *instr_p = (Instr*) ((AssignStringKeyInstr*) instr + 1);
      break;
    }
    case INSTR_SET_CONSTRAINT_STRING_KEY:
    {
      SetConstraintStringKeyInstr *sci = (SetConstraintStringKeyInstr*) instr;
      fprintf(stderr, "set constraint: %%%i . '%.*s' : %%%i \t\t(opt: string key)\n",
              sci->obj_slot, sci->key_len, sci->key_ptr, sci->constraint_slot);
      *instr_p = (Instr*) (sci + 1);
      break;
    }
    case INSTR_SET_SLOT:
    {
      SetSlotInstr *ssi = (SetSlotInstr*) instr;
      fprintf(stderr, "set slot: %%%i = %p \t\t (opt: %s)\n",
              ssi->target_slot, (void*) ssi->value, ssi->opt_info);
      *instr_p = (Instr*) (ssi + 1);
      break;
    }
    case INSTR_DEFINE_REFSLOT:
    {
      DefineRefslotInstr *dri = (DefineRefslotInstr*) instr;
      fprintf(stderr, "def refslot: &%i = %%%i . '%.*s' \t\t (opt: refslot)\n",
              dri->target_refslot, dri->obj_slot, dri->key_len, dri->key_ptr);
      *instr_p = (Instr*) (dri + 1);
      break;
    }
    case INSTR_READ_REFSLOT:
    {
      ReadRefslotInstr *rri = (ReadRefslotInstr*) instr;
      fprintf(stderr, "read refslot: %%%i = &%i \t\t (opt: %s)\n",
              rri->target_slot, rri->source_refslot, rri->opt_info);
      *instr_p = (Instr*) (rri + 1);
      break;
    }
    case INSTR_WRITE_REFSLOT:
    {
      WriteRefslotInstr *wri = (WriteRefslotInstr*) instr;
      fprintf(stderr, "write refslot: &%i = %%%i \t\t (opt: %s)\n",
              wri->target_refslot, wri->source_slot, wri->opt_info);
      *instr_p = (Instr*) (wri + 1);
      break;
    }
    case INSTR_ALLOC_STATIC_OBJECT:
    {
      AllocStaticObjectInstr *asoi = (AllocStaticObjectInstr*) instr;
      fprintf(stderr, "%%%i = new frame %%%i { ", asoi->target_slot, asoi->parent_slot);
      for (int i = 0; i < asoi->info_len; ++i) {
        StaticFieldInfo *info = &asoi->info_ptr[i];
        char *infostr = "";
        if (info->constraint) infostr = get_type_info(state, info->constraint);
        fprintf(stderr, "%.*s %s%s = %%%i (&%i); ",
                info->name_len, info->name_ptr, info->constraint?": ":"", infostr, info->slot, info->refslot);
      }
      fprintf(stderr, "}\n");
      *instr_p = (Instr*) (asoi + 1);
      break;
    }
    default:
      fprintf(stderr, "    unknown instruction: %i\n", instr->type);
      abort();
      break;
  }
}

void dump_fn(VMState *state, UserFunction *fn) {
  UserFunction **other_fns_ptr = NULL; int other_fns_len = 0;
  
  FunctionBody *body = &fn->body;
  fprintf(stderr, "function %s (%i), %i slots, %i refslots [\n", fn->name, fn->arity, fn->slots, fn->refslots);
  for (int i = 0; i < body->blocks_len; ++i) {
    fprintf(stderr, "  block <%i> [\n", i);
    Instr *instr = BLOCK_START(fn, i), *instr_end = BLOCK_END(fn, i);
    while (instr != instr_end) {
      if (instr->type == INSTR_ALLOC_CLOSURE_OBJECT) {
        other_fns_ptr = realloc(other_fns_ptr, sizeof(UserFunction*) * ++other_fns_len);
        other_fns_ptr[other_fns_len - 1] = ((AllocClosureObjectInstr*) instr)->fn;
      }
      dump_instr(state, &instr);
    }
    fprintf(stderr, "  ]\n");
  }
  fprintf(stderr, "]\n");
  
  for (int i = 0; i < other_fns_len; ++i) {
    fprintf(stderr, " ---\n");
    dump_fn(state, other_fns_ptr[i]);
  }
  free(other_fns_ptr);
}
