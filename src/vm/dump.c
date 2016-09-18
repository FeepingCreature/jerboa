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
      fprintf(stderr, "alloc object: %%%i = new object(%%%i, %s)\n",
              ((AllocObjectInstr*) instr)->target_slot, ((AllocObjectInstr*) instr)->parent_slot,
              ((AllocObjectInstr*) instr)->alloc_stack?"stack":"heap"
             );
      *instr_p = (Instr*) ((AllocObjectInstr*) instr + 1);
      break;
    case INSTR_ALLOC_INT_OBJECT:
      fprintf(stderr, "alloc int object: %s = new int(%i)\n",
              get_write_arg_info(((AllocIntObjectInstr*) instr)->target),
              ((AllocIntObjectInstr*) instr)->value);
      *instr_p = (Instr*) ((AllocIntObjectInstr*) instr + 1);
      break;
    case INSTR_ALLOC_BOOL_OBJECT:
    {
      AllocBoolObjectInstr *aboi = (AllocBoolObjectInstr*) instr;
      fprintf(stderr, "alloc bool object: %s = %s\n",
              get_write_arg_info(aboi->target),
              aboi->value?"true":"false");
      *instr_p = (Instr*) (aboi + 1);
      break;
    }
    case INSTR_ALLOC_FLOAT_OBJECT:
      fprintf(stderr, "alloc float object: %s = new float(%f)\n",
              get_write_arg_info(((AllocFloatObjectInstr*) instr)->target),
              ((AllocFloatObjectInstr*) instr)->value);
      *instr_p = (Instr*) ((AllocFloatObjectInstr*) instr + 1);
      break;
    case INSTR_ALLOC_ARRAY_OBJECT:
      fprintf(stderr, "alloc array object: %s = []\n",
              get_write_arg_info(((AllocArrayObjectInstr*) instr)->target));
      *instr_p = (Instr*) ((AllocArrayObjectInstr*) instr + 1);
      break;
    case INSTR_ALLOC_STRING_OBJECT:
      fprintf(stderr, "alloc string object: %s = new string(%s)\n",
              get_write_arg_info(((AllocStringObjectInstr*) instr)->target),
              ((AllocStringObjectInstr*) instr)->value);
      *instr_p = (Instr*) ((AllocStringObjectInstr*) instr + 1);
      break;
    case INSTR_ALLOC_CLOSURE_OBJECT:
      fprintf(stderr, "alloc closure object: %s = new function(), dumped later\n",
              get_write_arg_info(((AllocClosureObjectInstr*) instr)->target));
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
      fprintf(stderr, "access: %s = %s . %s\n",
              get_write_arg_info(((AccessInstr*) instr)->target),
              get_arg_info_ext(state, ((AccessInstr*) instr)->obj),
              get_arg_info_ext(state, ((AccessInstr*) instr)->key));
      *instr_p = (Instr*) ((AccessInstr*) instr + 1);
      break;
    case INSTR_ASSIGN:
    {
      char *mode = "(plain)";
      if (((AssignInstr*) instr)->type == ASSIGN_EXISTING) mode = "(existing)";
      else if (((AssignInstr*) instr)->type == ASSIGN_SHADOWING) mode = "(shadowing)";
      fprintf(stderr, "assign%s: (%%%i=) %s . %s = %s\n",
              mode, ((AssignInstr*) instr)->target_slot,
              get_arg_info_ext(state, ((AssignInstr*) instr)->obj),
              get_arg_info_ext(state, ((AssignInstr*) instr)->key),
              get_arg_info_ext(state, ((AssignInstr*) instr)->value));
      *instr_p = (Instr*) ((AssignInstr*) instr + 1);
      break;
    }
    case INSTR_KEY_IN_OBJ:
      fprintf(stderr, "key in obj: %s = %s in %s\n",
              get_write_arg_info(((KeyInObjInstr*) instr)->target),
              get_arg_info_ext(state, ((KeyInObjInstr*) instr)->key),
              get_arg_info_ext(state, ((KeyInObjInstr*) instr)->obj));
      *instr_p = (Instr*) ((KeyInObjInstr*) instr + 1);
      break;
    case INSTR_IDENTICAL:
      fprintf(stderr, "%s = %s == %s\n",
              get_write_arg_info(((IdenticalInstr*) instr)->target),
              get_arg_info_ext(state, ((IdenticalInstr*) instr)->obj1),
              get_arg_info_ext(state, ((IdenticalInstr*) instr)->obj2));
      *instr_p = (Instr*) ((IdenticalInstr*) instr + 1);
      break;
    case INSTR_INSTANCEOF:
      fprintf(stderr, "instance of: %s = %s instanceof %s\n",
              get_write_arg_info(((InstanceofInstr*) instr)->target),
              get_arg_info_ext(state, ((InstanceofInstr*) instr)->obj),
              get_arg_info_ext(state, ((InstanceofInstr*) instr)->proto));
      *instr_p = (Instr*) ((InstanceofInstr*) instr + 1);
      break;
    case INSTR_SET_CONSTRAINT:
    {
      SetConstraintInstr *sci = (SetConstraintInstr*) instr;
      fprintf(stderr, "set constraint: %s . %s : %s\n",
              get_arg_info_ext(state, sci->obj),
              get_arg_info_ext(state, sci->key),
              get_arg_info_ext(state, sci->constraint));
      *instr_p = (Instr*) (sci + 1);
      break;
    }
    case INSTR_CALL:
    {
      CallInstr *ci = (CallInstr*) instr;
      fprintf(stderr, "call: %s = %s . %s ( ",
              get_write_arg_info(ci->info.target), get_arg_info_ext(state, ci->info.this_arg), get_arg_info_ext(state, ci->info.fn));
      for (int i = 0; i < ci->info.args_len; ++i) {
        if (i) fprintf(stderr, ", ");
        fprintf(stderr, "%s", get_arg_info_ext(state, ((Arg*)(ci + 1))[i]));
      }
      fprintf(stderr, " )\n");
      *instr_p = (Instr*) ((char*)ci + ci->size);
      break;
    }
    case INSTR_CALL_FUNCTION_DIRECT:
    {
      CallFunctionDirectInstr *cfdi = (CallFunctionDirectInstr*) instr;
      fprintf(stderr, "call intrinsic: %s = %s . %s ( ",
              get_write_arg_info(cfdi->info.target), get_arg_info_ext(state, cfdi->info.this_arg), get_arg_info_ext(state, cfdi->info.fn));
      for (int i = 0; i < cfdi->info.args_len; ++i) {
        if (i) fprintf(stderr, ", ");
        fprintf(stderr, "%s", get_arg_info_ext(state, ((Arg*)(cfdi + 1))[i]));
      }
      fprintf(stderr, " ) \t (opt: fast path)\n");
      *instr_p = (Instr*) ((char*)cfdi + cfdi->size);
      break;
    }
    case INSTR_RETURN:
      fprintf(stderr, "return: %s\n", get_arg_info_ext(state, ((ReturnInstr*) instr)->ret));
      *instr_p = (Instr*) ((ReturnInstr*) instr + 1);
      break;
    case INSTR_BR:
      fprintf(stderr, "branch: <%i>\n", ((BranchInstr*) instr)->blk);
      *instr_p = (Instr*) ((BranchInstr*) instr + 1);
      break;
    case INSTR_TESTBR:
      fprintf(stderr, "test-branch: %s ? <%i> : <%i>\n",
              get_arg_info_ext(state, ((TestBranchInstr*) instr)->test),
              ((TestBranchInstr*) instr)->true_blk, ((TestBranchInstr*) instr)->false_blk);
      *instr_p = (Instr*) ((TestBranchInstr*) instr + 1);
      break;
    case INSTR_PHI:
    {
      PhiInstr *phi = (PhiInstr*) instr;
      fprintf(stderr, "phi: %s = [ <%i>: %s, <%i>: %s ]\n",
              get_write_arg_info(phi->target),
              phi->block1, get_arg_info_ext(state, phi->arg1),
              phi->block2, get_arg_info_ext(state, phi->arg2));
      *instr_p = (Instr*) (phi + 1);
      break;
    }
    case INSTR_ACCESS_STRING_KEY:
      fprintf(stderr, "access: %s = %s . '%.*s' \t\t(opt: string key, scratch %%%i)\n",
              get_write_arg_info(((AccessStringKeyInstr*) instr)->target),
              get_arg_info_ext(state, ((AccessStringKeyInstr*) instr)->obj),
              (int) ((AccessStringKeyInstr*) instr)->key.len, ((AccessStringKeyInstr*) instr)->key.ptr,
              ((AccessStringKeyInstr*) instr)->key_slot);
      *instr_p = (Instr*) ((AccessStringKeyInstr*) instr + 1);
      break;
    case INSTR_ASSIGN_STRING_KEY:
    {
      char *mode = "(plain)";
      if (((AssignStringKeyInstr*) instr)->type == ASSIGN_EXISTING) mode = "(existing)";
      else if (((AssignStringKeyInstr*) instr)->type == ASSIGN_SHADOWING) mode = "(shadowing)";
      fprintf(stderr, "assign%s: (%i=) %s . '%.*s' = %s \t\t(opt: string key)\n",
              mode,
              ((AssignStringKeyInstr*) instr)->target_slot,
              get_arg_info_ext(state, ((AssignStringKeyInstr*) instr)->obj),
              (int) ((AssignStringKeyInstr*) instr)->key.len, ((AssignStringKeyInstr*) instr)->key.ptr,
              get_arg_info_ext(state, ((AssignStringKeyInstr*) instr)->value));
      *instr_p = (Instr*) ((AssignStringKeyInstr*) instr + 1);
      break;
    }
    case INSTR_SET_CONSTRAINT_STRING_KEY:
    {
      SetConstraintStringKeyInstr *sci = (SetConstraintStringKeyInstr*) instr;
      fprintf(stderr, "set constraint: %s . '%.*s' : %s \t\t(opt: string key)\n",
              get_arg_info_ext(state, sci->obj),
              sci->key_len, sci->key_ptr,
              get_arg_info_ext(state, sci->constraint));;
      *instr_p = (Instr*) (sci + 1);
      break;
    }
    case INSTR_DEFINE_REFSLOT:
    {
      DefineRefslotInstr *dri = (DefineRefslotInstr*) instr;
      fprintf(stderr, "def refslot: &%i = %%%i . '%.*s' \t\t (opt: refslot)\n",
              dri->target_refslot, dri->obj_slot, (int) dri->key.len, dri->key.ptr);
      *instr_p = (Instr*) (dri + 1);
      break;
    }
    case INSTR_MOVE:
    {
      MoveInstr *mi = (MoveInstr*) instr;
      fprintf(stderr, "move: %s = %s \t %s\n",
              get_write_arg_info(mi->target), get_arg_info_ext(state, mi->source), mi->opt_info);
      *instr_p = (Instr*) (mi + 1);
      break;
    }
    case INSTR_ALLOC_STATIC_OBJECT:
    {
      AllocStaticObjectInstr *asoi = (AllocStaticObjectInstr*) instr;
      fprintf(stderr, "%%%i = new frame %%%i { ", asoi->target_slot, asoi->parent_slot);
      for (int i = 0; i < asoi->info_len; ++i) {
        StaticFieldInfo *info = &ASOI_INFO(asoi)[i];
        char *infostr = "";
        if (info->constraint) infostr = get_type_info(state, OBJ2VAL(info->constraint));
        fprintf(stderr, "%.*s%s%s = %%%i (&%i); ",
                (int) info->name.len, info->name.ptr, info->constraint?": ":"", infostr, info->slot, info->refslot);
      }
      fprintf(stderr, "} %s\n", asoi->alloc_stack?"stack":"heap");
      *instr_p = (Instr*) ((char*) instr + instr_size(instr));
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
