#include "vm/dump.h"

#include <stdio.h>

void dump_instr(Instr **instr_p) {
  Instr *instr = *instr_p;
  // fprintf(stderr, "%p", (void*) instr);
  switch (instr->type) {
    case INSTR_GET_ROOT:
      fprintf(stderr, "    get_root: %i\n", ((GetRootInstr*) instr)->slot);
      *instr_p = (Instr*) ((GetRootInstr*) instr + 1);
      break;
    case INSTR_GET_CONTEXT:
      fprintf(stderr, "    get_context: %i\n", ((GetContextInstr*) instr)->slot);
      *instr_p = (Instr*) ((GetContextInstr*) instr + 1);
      break;
    case INSTR_ALLOC_OBJECT:
      fprintf(stderr, "    alloc_object: %i = new object(%i)\n",
              ((AllocObjectInstr*) instr)->target_slot, ((AllocObjectInstr*) instr)->parent_slot);
      *instr_p = (Instr*) ((AllocObjectInstr*) instr + 1);
      break;
    case INSTR_ALLOC_INT_OBJECT:
      fprintf(stderr, "    alloc_int_object: %i = new int(%i)\n",
              ((AllocIntObjectInstr*) instr)->target_slot, ((AllocIntObjectInstr*) instr)->value);
      *instr_p = (Instr*) ((AllocIntObjectInstr*) instr + 1);
      break;
    case INSTR_ALLOC_FLOAT_OBJECT:
      fprintf(stderr, "    alloc_float_object: %i = new float(%f)\n",
              ((AllocFloatObjectInstr*) instr)->target_slot, ((AllocFloatObjectInstr*) instr)->value);
      *instr_p = (Instr*) ((AllocFloatObjectInstr*) instr + 1);
      break;
    case INSTR_ALLOC_ARRAY_OBJECT:
      fprintf(stderr, "    alloc_array_object: %i = []\n",
              ((AllocArrayObjectInstr*) instr)->target_slot);
      *instr_p = (Instr*) ((AllocArrayObjectInstr*) instr + 1);
      break;
    case INSTR_ALLOC_STRING_OBJECT:
      fprintf(stderr, "    alloc_string_object: %i = new string(%s)\n",
              ((AllocStringObjectInstr*) instr)->target_slot, ((AllocStringObjectInstr*) instr)->value);
      *instr_p = (Instr*) ((AllocStringObjectInstr*) instr + 1);
      break;
    case INSTR_ALLOC_CLOSURE_OBJECT:
      fprintf(stderr, "    alloc_closure_object: %i = new function(%i), dumped later\n",
              ((AllocClosureObjectInstr*) instr)->target_slot, ((AllocClosureObjectInstr*) instr)->context_slot);
      *instr_p = (Instr*) ((AllocClosureObjectInstr*) instr + 1);
      break;
    case INSTR_CLOSE_OBJECT:
      fprintf(stderr, "    close_object: %i\n", ((CloseObjectInstr*) instr)->slot);
      *instr_p = (Instr*) ((CloseObjectInstr*) instr + 1);
      break;
    case INSTR_FREEZE_OBJECT:
      fprintf(stderr, "    freeze_object: %i\n", ((FreezeObjectInstr*) instr)->slot);
      *instr_p = (Instr*) ((FreezeObjectInstr*) instr + 1);
      break;
    case INSTR_ACCESS:
      fprintf(stderr, "    access: %i = %i . %i\n",
              ((AccessInstr*) instr)->target_slot, ((AccessInstr*) instr)->obj_slot, ((AccessInstr*) instr)->key_slot);
      *instr_p = (Instr*) ((AccessInstr*) instr + 1);
      break;
    case INSTR_ASSIGN:
    {
      char *mode = "(plain)";
      if (((AssignInstr*) instr)->type == ASSIGN_EXISTING) mode = "(existing)";
      else if (((AssignInstr*) instr)->type == ASSIGN_SHADOWING) mode = "(shadowing)";
      fprintf(stderr, "    assign%s: %i . %i = %i\n",
              mode, ((AssignInstr*) instr)->obj_slot, ((AssignInstr*) instr)->key_slot, ((AssignInstr*) instr)->value_slot);
      *instr_p = (Instr*) ((AssignInstr*) instr + 1);
      break;
    }
    case INSTR_CALL:
      fprintf(stderr, "    call: %i . %i ( ",
              ((CallInstr*) instr)->this_slot, ((CallInstr*) instr)->function_slot);
      for (int i = 0; i < ((CallInstr*) instr)->args_length; ++i) {
        if (i) fprintf(stderr, ", ");
        fprintf(stderr, "%i", ((CallInstr*) instr)->args_ptr[i]);
      }
      fprintf(stderr, " )\n");
      *instr_p = (Instr*) ((CallInstr*) instr + 1);
      break;
    case INSTR_RETURN:
      fprintf(stderr, "    return: %i\n", ((ReturnInstr*) instr)->ret_slot);
      *instr_p = (Instr*) ((ReturnInstr*) instr + 1);
      break;
    case INSTR_SAVE_RESULT:
      fprintf(stderr, "    save result: -> %i\n",
              ((SaveResultInstr*) instr)->target_slot);
      *instr_p = (Instr*) ((SaveResultInstr*) instr + 1);
      break;
    case INSTR_BR:
      fprintf(stderr, "    branch: <%i>\n", ((BranchInstr*) instr)->blk);
      *instr_p = (Instr*) ((BranchInstr*) instr + 1);
      break;
    case INSTR_TESTBR:
      fprintf(stderr, "    test-branch: %i ? <%i> : <%i>\n",
              ((TestBranchInstr*) instr)->test_slot, ((TestBranchInstr*) instr)->true_blk, ((TestBranchInstr*) instr)->false_blk);
      *instr_p = (Instr*) ((TestBranchInstr*) instr + 1);
      break;
    case INSTR_ACCESS_STRING_KEY:
      fprintf(stderr, "    access: %i = %i . '%s' \t\t(opt: string key)\n",
              ((AccessStringKeyInstr*) instr)->target_slot, ((AccessStringKeyInstr*) instr)->obj_slot, ((AccessStringKeyInstr*) instr)->key);
      *instr_p = (Instr*) ((AccessStringKeyInstr*) instr + 1);
      break;
    case INSTR_ASSIGN_STRING_KEY:
    {
      char *mode = "(plain)";
      if (((AssignStringKeyInstr*) instr)->type == ASSIGN_EXISTING) mode = "(existing)";
      else if (((AssignStringKeyInstr*) instr)->type == ASSIGN_SHADOWING) mode = "(shadowing)";
      fprintf(stderr, "    assign%s: %i . '%s' = %i \t\t(opt: string key)\n",
              mode, ((AssignStringKeyInstr*) instr)->obj_slot, ((AssignStringKeyInstr*) instr)->key, ((AssignStringKeyInstr*) instr)->value_slot);
      *instr_p = (Instr*) ((AssignStringKeyInstr*) instr + 1);
      break;
    }
    default:
      fprintf(stderr, "    unknown instruction: %i\n", instr->type);
      assert(false);
      break;
  }
}

void dump_fn(UserFunction *fn) {
  UserFunction **other_fns_ptr = NULL; int other_fns_len = 0;
  
  FunctionBody *body = &fn->body;
  fprintf(stderr, "function %s (%i), %i slots [\n", fn->name, fn->arity, fn->slots);
  for (int i = 0; i < body->blocks_len; ++i) {
    fprintf(stderr, "  block <%i> [\n", i);
    InstrBlock *block = &body->blocks_ptr[i];
    Instr *instr = block->instrs_ptr;
    while (instr != block->instrs_ptr_end) {
      if (instr->type == INSTR_ALLOC_CLOSURE_OBJECT) {
        other_fns_ptr = realloc(other_fns_ptr, sizeof(UserFunction*) * ++other_fns_len);
        other_fns_ptr[other_fns_len - 1] = ((AllocClosureObjectInstr*) instr)->fn;
      }
      dump_instr(&instr);
    }
    fprintf(stderr, "  ]\n");
  }
  fprintf(stderr, "]\n");
  
  for (int i = 0; i < other_fns_len; ++i) {
    fprintf(stderr, " ---\n");
    dump_fn(other_fns_ptr[i]);
  }
  free(other_fns_ptr);
}
