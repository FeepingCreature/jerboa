#include "vm/dump.h"

#include <stdio.h>

void dump_fn(UserFunction *fn) {
  UserFunction **other_fns_ptr = NULL; int other_fns_len = 0;
  
  FunctionBody *body = &fn->body;
  fprintf(stderr, "function %s (%i), %i slots [\n", fn->name, fn->arity, fn->slots);
  for (int i = 0; i < body->blocks_len; ++i) {
    fprintf(stderr, "  block <%i> [\n", i);
    InstrBlock *block = &body->blocks_ptr[i];
    for (int k = 0; k < block->instrs_len; ++k) {
      Instr *instr = block->instrs_ptr[k];
      switch (instr->type) {
        case INSTR_GET_ROOT:
          fprintf(stderr, "    get_root: %i\n", ((GetRootInstr*) instr)->slot);
          break;
        case INSTR_GET_CONTEXT:
          fprintf(stderr, "    get_context: %i\n", ((GetContextInstr*) instr)->slot);
          break;
        case INSTR_ALLOC_OBJECT:
          fprintf(stderr, "    alloc_object: %i = new object(%i)\n",
                  ((AllocObjectInstr*) instr)->target_slot, ((AllocObjectInstr*) instr)->parent_slot);
          break;
        case INSTR_ALLOC_INT_OBJECT:
          fprintf(stderr, "    alloc_int_object: %i = new int(%i)\n",
                  ((AllocIntObjectInstr*) instr)->target_slot, ((AllocIntObjectInstr*) instr)->value);
          break;
        case INSTR_ALLOC_CLOSURE_OBJECT:
          fprintf(stderr, "    alloc_closure_object: %i = new function(%i), dumped later\n",
                  ((AllocClosureObjectInstr*) instr)->target_slot, ((AllocClosureObjectInstr*) instr)->context_slot);
          other_fns_ptr = realloc(other_fns_ptr, sizeof(UserFunction*) * ++other_fns_len);
          other_fns_ptr[other_fns_len - 1] = ((AllocClosureObjectInstr*) instr)->fn;
          break;
        case INSTR_CLOSE_OBJECT:
          fprintf(stderr, "    close_object: %i\n", ((CloseObjectInstr*) instr)->slot);
          break;
        case INSTR_ACCESS:
          fprintf(stderr, "    access: %i = %i . '%s'\n",
                  ((AccessInstr*) instr)->target_slot, ((AccessInstr*) instr)->obj_slot, ((AccessInstr*) instr)->key);
          break;
        case INSTR_ASSIGN:
          fprintf(stderr, "    assign: %i . '%s' = %i\n",
                  ((AssignInstr*) instr)->obj_slot, ((AssignInstr*) instr)->key, ((AssignInstr*) instr)->value_slot);
          break;
        case INSTR_ASSIGN_EXISTING:
          fprintf(stderr, "    assign_existing: %i . '%s' = %i\n",
                  ((AssignExistingInstr*) instr)->obj_slot, ((AssignExistingInstr*) instr)->key, ((AssignExistingInstr*) instr)->value_slot);
          break;
        case INSTR_CALL:
          fprintf(stderr, "    call: %i = %i ( ",
                  ((CallInstr*) instr)->target_slot, ((CallInstr*) instr)->function_slot);
          for (int i = 0; i < ((CallInstr*) instr)->args_length; ++i) {
            if (i) fprintf(stderr, ", ");
            fprintf(stderr, "%i", ((CallInstr*) instr)->args_ptr[i]);
          }
          fprintf(stderr, " )\n");
          break;
        case INSTR_RETURN:
          fprintf(stderr, "    return: %i\n", ((ReturnInstr*) instr)->ret_slot);
          break;
        case INSTR_BR:
          fprintf(stderr, "    branch: <%i>\n", ((BranchInstr*) instr)->blk);
          break;
        case INSTR_TESTBR:
          fprintf(stderr, "    test-branch: %i ? <%i> : <%i>\n",
                  ((TestBranchInstr*) instr)->test_slot, ((TestBranchInstr*) instr)->true_blk, ((TestBranchInstr*) instr)->false_blk);
          break;
        default: fprintf(stderr, "    unknown instruction: %i\n", instr->type); break;
      }
    }
    fprintf(stderr, "  ]\n");
  }
  fprintf(stderr, "]\n");
  
  for (int i = 0; i < other_fns_len; ++i) {
    fprintf(stderr, " ---\n");
    dump_fn(other_fns_ptr[i]);
  }
}
