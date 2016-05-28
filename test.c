#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdarg.h>
#include <assert.h>

#include "object.h"
#include "vm/call.h"
#include "vm/runtime.h"

#define alloc(T, ...) ({ T *ptr = malloc(sizeof(T)); *ptr = ((T) __VA_ARGS__); ptr; })

int main(int argc, char **argv) {
  Object *root = create_root();
  
  UserFunction *ack_fn = malloc(sizeof(UserFunction));
  ack_fn->arity = 2;
  ack_fn->slots = 24;
  ack_fn->name = "ack";
  ack_fn->body.blocks_len = 5;
  ack_fn->body.blocks_ptr = malloc(sizeof(InstrBlock) * 5);
  InstrBlock *blocks_ptr = ack_fn->body.blocks_ptr;
  blocks_ptr[0].instrs_len = 5;
  blocks_ptr[0].instrs_ptr = malloc(sizeof(Instr*) * 5);
  blocks_ptr[0].instrs_ptr[0] = (Instr*) alloc(AllocIntObjectInstr, {{INSTR_ALLOC_INT_OBJECT}, 4, 0});
  blocks_ptr[0].instrs_ptr[1] = (Instr*) alloc(GetContextInstr, {{INSTR_GET_CONTEXT}, 5});
  blocks_ptr[0].instrs_ptr[2] = (Instr*) alloc(AccessInstr, {{INSTR_ACCESS}, 6, 5, "="});
  blocks_ptr[0].instrs_ptr[3] = (Instr*) alloc(CallInstr, {{INSTR_CALL}, 7, 6, (int[]) {0, 4}, 2});
  blocks_ptr[0].instrs_ptr[4] = (Instr*) alloc(TestBranchInstr, {{INSTR_TESTBR}, 7, 1, 2});
  blocks_ptr[1].instrs_len = 4;
  blocks_ptr[1].instrs_ptr = malloc(sizeof(Instr*) * 4);
  blocks_ptr[1].instrs_ptr[0] = (Instr*) alloc(AccessInstr, {{INSTR_ACCESS}, 8, 5, "+"});
  blocks_ptr[1].instrs_ptr[1] = (Instr*) alloc(AllocIntObjectInstr, {{INSTR_ALLOC_INT_OBJECT}, 9, 1});
  blocks_ptr[1].instrs_ptr[2] = (Instr*) alloc(CallInstr, {{INSTR_CALL}, 10, 8, (int[]) {1, 9}, 2});
  blocks_ptr[1].instrs_ptr[3] = (Instr*) alloc(ReturnInstr, {{INSTR_RETURN}, 10});
  blocks_ptr[2].instrs_len = 2;
  blocks_ptr[2].instrs_ptr = malloc(sizeof(Instr*) * 2);
  blocks_ptr[2].instrs_ptr[0] = (Instr*) alloc(CallInstr, {{INSTR_CALL}, 11, 6, (int[]) {1, 4}, 2});
  blocks_ptr[2].instrs_ptr[1] = (Instr*) alloc(TestBranchInstr, {{INSTR_TESTBR}, 11, 3, 4});
  blocks_ptr[3].instrs_len = 6;
  blocks_ptr[3].instrs_ptr = malloc(sizeof(Instr*) * 6);
  blocks_ptr[3].instrs_ptr[0] = (Instr*) alloc(AccessInstr, {{INSTR_ACCESS}, 12, 5, "-"});
  blocks_ptr[3].instrs_ptr[1] = (Instr*) alloc(AllocIntObjectInstr, {{INSTR_ALLOC_INT_OBJECT}, 13, 1});
  blocks_ptr[3].instrs_ptr[2] = (Instr*) alloc(CallInstr, {{INSTR_CALL}, 14, 12, (int[]) {0, 13}, 2});
  blocks_ptr[3].instrs_ptr[3] = (Instr*) alloc(AccessInstr, {{INSTR_ACCESS}, 15, 5, "ack"});
  blocks_ptr[3].instrs_ptr[4] = (Instr*) alloc(CallInstr, {{INSTR_CALL}, 16, 15, (int[]) {14, 13}, 2});
  blocks_ptr[3].instrs_ptr[5] = (Instr*) alloc(ReturnInstr, {{INSTR_RETURN}, 16});
  blocks_ptr[4].instrs_len = 8;
  blocks_ptr[4].instrs_ptr = malloc(sizeof(Instr*) * 8);
  blocks_ptr[4].instrs_ptr[0] = (Instr*) alloc(AccessInstr, {{INSTR_ACCESS}, 17, 5, "-"});
  blocks_ptr[4].instrs_ptr[1] = (Instr*) alloc(AllocIntObjectInstr, {{INSTR_ALLOC_INT_OBJECT}, 18, 1});
  blocks_ptr[4].instrs_ptr[2] = (Instr*) alloc(CallInstr, {{INSTR_CALL}, 19, 17, (int[]) {0, 18}, 2});
  blocks_ptr[4].instrs_ptr[3] = (Instr*) alloc(CallInstr, {{INSTR_CALL}, 20, 17, (int[]) {1, 18}, 2});
  blocks_ptr[4].instrs_ptr[4] = (Instr*) alloc(AccessInstr, {{INSTR_ACCESS}, 21, 5, "ack"});
  blocks_ptr[4].instrs_ptr[5] = (Instr*) alloc(CallInstr, {{INSTR_CALL}, 22, 21, (int[]) {0, 20}, 2});
  blocks_ptr[4].instrs_ptr[6] = (Instr*) alloc(CallInstr, {{INSTR_CALL}, 23, 21, (int[]) {19, 22}, 2});
  blocks_ptr[4].instrs_ptr[7] = (Instr*) alloc(ReturnInstr, {{INSTR_RETURN}, 23});
  Object *ack = alloc_user_fn(root, ack_fn);
  object_set(root, "ack", ack);
  /*
  blocks_ptr[0].instrs_len = 7;
  blocks_ptr[0].instrs_ptr = malloc(sizeof(Instr*) * 7);
  blocks_ptr[0].instrs_ptr[0] = (Instr*) alloc(GetRootInstr, {{INSTR_GET_ROOT}, 2});
  blocks_ptr[0].instrs_ptr[1] = (Instr*) alloc(AccessInstr, {{INSTR_ACCESS}, 3, 2, "const_ints"});
  blocks_ptr[0].instrs_ptr[2] = (Instr*) alloc(AccessInstr, {{INSTR_ACCESS}, 4, 3, "int1"});
  blocks_ptr[0].instrs_ptr[3] = (Instr*) alloc(GetContextInstr, {{INSTR_GET_CONTEXT}, 5});
  blocks_ptr[0].instrs_ptr[4] = (Instr*) alloc(AccessInstr, {{INSTR_ACCESS}, 6, 5, "="});
  blocks_ptr[0].instrs_ptr[5] = (Instr*) alloc(CallInstr, {{INSTR_CALL}, 7, 6, (int[]) {0, 4}, 2});
  blocks_ptr[0].instrs_ptr[6] = (Instr*) alloc(TestBranchInstr, {{INSTR_TESTBR}, 7, 1, 2});
  blocks_ptr[1].instrs_len = 1;
  blocks_ptr[1].instrs_ptr = malloc(sizeof(Instr*) * 1);
  blocks_ptr[1].instrs_ptr[0] = (Instr*) alloc(ReturnInstr, {{INSTR_RETURN}, 4});
  blocks_ptr[2].instrs_len = 8;
  blocks_ptr[2].instrs_ptr = malloc(sizeof(Instr*) * 8);
  blocks_ptr[2].instrs_ptr[0] = (Instr*) alloc(GetContextInstr, {{INSTR_GET_CONTEXT}, 8});
  blocks_ptr[2].instrs_ptr[1] = (Instr*) alloc(AccessInstr, {{INSTR_ACCESS}, 9, 8, "-"});
  blocks_ptr[2].instrs_ptr[2] = (Instr*) alloc(CallInstr, {{INSTR_CALL}, 10, 9, (int[]) {0, 4}, 2});
  blocks_ptr[2].instrs_ptr[3] = (Instr*) alloc(AccessInstr, {{INSTR_ACCESS}, 11, 8, "fac"});
  blocks_ptr[2].instrs_ptr[4] = (Instr*) alloc(CallInstr, {{INSTR_CALL}, 12, 11, (int[]) {10}, 1});
  blocks_ptr[2].instrs_ptr[5] = (Instr*) alloc(AccessInstr, {{INSTR_ACCESS}, 13, 8, "*"});
  blocks_ptr[2].instrs_ptr[6] = (Instr*) alloc(CallInstr, {{INSTR_CALL}, 14, 13, (int[]) {0, 12}, 2});
  blocks_ptr[2].instrs_ptr[7] = (Instr*) alloc(ReturnInstr, {{INSTR_RETURN}, 14});
  Object *fac = alloc_user_fn(root, fac_fn);
  object_set(root, "fac", fac);
  */
  
  Object **args_ptr = malloc(sizeof(Object*) * 2);
  args_ptr[0] = alloc_int(root, 3);
  args_ptr[1] = alloc_int(root, 7);
  Object *res = user_function_handler(root, ack, args_ptr, 2);
  IntObject *res_int = (IntObject*) res;
  printf("ack(3, 7) = %i\n", res_int->value);
  return 0;
}
