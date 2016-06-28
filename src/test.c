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
  (void) argc; assert(argc == 2);
  
  init_instr_fn_table();
  
  VMState vmstate = {0};
  vmstate.shared = calloc(sizeof(VMSharedState), 1);
  gc_init(&vmstate);
  
  vm_alloc_frame(&vmstate, 0);
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
  dump_fn(module);
  
  call_function(&vmstate, root, module, NULL, 0);
  vm_run(&vmstate);
  
  save_profile_output("profile.html", source, &vmstate.shared->profstate);
  
  if (vmstate.runstate == VM_ERRORED) {
    fprintf(stderr, "at:\n");
    vm_print_backtrace(&vmstate);
    fprintf(stderr, "vm failure: %s\n", vmstate.error);
  }
  
  printf("(%i cycles)\n", vmstate.shared->cyclecount);
  
  gc_remove_roots(&vmstate, &set);
  
  // one last run, deleting everything
  gc_run(&vmstate);
  
  return 0;
}
