#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <assert.h>

#include "object.h"
#include "gc.h"
#include "vm/call.h"
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
  vmstate.shared = calloc(sizeof(VMSharedState), 1);
  
  int argc2 = 0;
  char **argv2 = NULL;
  for (int i = 0; i < argc; ++i) {
    if (i > 0 && strcmp(argv[i], "-v") == 0) {
      vmstate.shared->verbose = true;
    } else {
      argv2 = realloc(argv2, sizeof(char*) * ++argc2);
      argv2[argc2 - 1] = argv[i];
    }
  }
  argc = argc2;
  argv = argv2;
  
  init_instr_fn_table();
  
  vm_alloc_frame(&vmstate, 0, 0);
  Object *root = create_root(&vmstate);
  vm_remove_frame(&vmstate);
  
  GCRootSet set;
  gc_add_roots(&vmstate, &root, 1, &set);
  
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
  Object **args_ptr = malloc(sizeof(Object*) * args_len);
  for (int i = 2; i < argc; ++i) {
    args_ptr[i - 2] = alloc_string(&vmstate, argv[i], strlen(argv[i]));
  }
  
  Object *args = alloc_array(&vmstate, args_ptr, alloc_int(&vmstate, args_len));
  object_set(root, "arguments", args);
  
  if (vmstate.shared->verbose) {
    dump_fn(&vmstate, module);
  }
  
  call_function(&vmstate, root, module, NULL, 0);
  vm_run(&vmstate);
  
  save_profile_output("profile.html", source, &vmstate.shared->profstate);
  
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
  
  return resvalue;
}
