#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <assert.h>

#include "object.h"
#include "gc.h"
#include "vm/call.h"
#include "vm/optimize.h"
#include "vm/runtime.h"
#include "vm/dump.h"
#include "vm/vm.h"
#include "language.h"
#include "util.h"

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "expected: jerboa [script file] [arguments]\n");
    return 1;
  }
  
  VMState vmstate = {0};
  vmstate.runstate = VM_TERMINATED;
  vmstate.shared = calloc(sizeof(VMSharedState), 1);
  vmstate.shared->stack_data_len = 16*1024*1024;
  vmstate.shared->stack_data_ptr = malloc(vmstate.shared->stack_data_len);
  vmstate.shared->gcstate.head.next = &vmstate.shared->gcstate.tail;
  vmstate.shared->gcstate.tail.prev = &vmstate.shared->gcstate.head;
  
  int argc2 = 0;
  char **argv2 = NULL;
  for (int i = 0; i < argc; ++i) {
    if (i > 0 && strcmp(argv[i], "-v") == 0) {
      vmstate.shared->verbose = true;
    } else if (i > 0 && strcmp(argv[i], "-pg") == 0) {
      vmstate.shared->profstate.profiling_enabled = true;
    } else {
      argv2 = realloc(argv2, sizeof(char*) * ++argc2);
      argv2[argc2 - 1] = argv[i];
    }
  }
  argc = argc2;
  argv = argv2;
  
  init_instr_fn_table();
  
  Object *root = create_root(&vmstate);
  Value rootval = OBJ2VAL(root);
  
  GCRootSet set;
  gc_add_roots(&vmstate, &rootval, 1, &set);
  
  TextRange source = readfile(argv[1]);
  register_file(source, argv[1], 0, 0);
  
  UserFunction *module;
  char *text = source.start;
  ParseResult res = parse_module(&text, &module);
  if (res != PARSE_OK) {
    return 1;
  }
  assert(res == PARSE_OK);
  
  int args_len = argc - 2;
  Value *args_ptr = malloc(sizeof(Value) * args_len);
  for (int i = 2; i < argc; ++i) {
    args_ptr[i - 2] = make_string(&vmstate, argv[i], strlen(argv[i]));
  }
  
  Value args = make_array(&vmstate, args_ptr, args_len, true);
  OBJECT_SET_STRING(&vmstate, root, "arguments", args);
  
  if (vmstate.shared->verbose) {
    dump_fn(&vmstate, module);
  }
  
  module = optimize_runtime(&vmstate, module, root);
  
  Value retval;
  
  CallInfo info = {{0}};
  info.target = (WriteArg) { .kind = ARG_POINTER, .pointer = &retval };
  call_function(&vmstate, root, module, &info);
  vm_update_frame(&vmstate);
  vm_run(&vmstate);
  
  if (vmstate.shared->profstate.profiling_enabled) {
    save_profile_output("profile.cg", &vmstate.shared->profstate);
  }
  
  int resvalue = 0;
  if (vmstate.runstate == VM_ERRORED) {
    fprintf(stderr, "at:\n");
    vm_print_backtrace(&vmstate);
    fprintf(stderr, "vm failure: %s\n", vmstate.error);
    resvalue = 1;
  }
  
  if (vmstate.shared->verbose) {
    printf("(%i cycles)\n", vmstate.shared->cyclecount);
  }
  
  gc_remove_roots(&vmstate, &set);
  
  // one last run, deleting everything
  gc_run(&vmstate);
  
  if (vmstate.shared->verbose) {
    fprintf(stderr, "(%i bytes remaining)\n", vmstate.shared->gcstate.bytes_allocated);
  }
  
  return resvalue;
}
