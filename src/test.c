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
#include "language.h"
#include "util.h"

int main(int argc, char **argv) {
  assert(argc == 2);
  
  VMState vmstate = {0};
  vmstate.gcstate = calloc(sizeof(GCState), 1);
  vmstate.profstate = calloc(sizeof(VMProfileState), 1);
  vmstate.vcache = calloc(sizeof(ValueCache), 1);
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
  
  save_profile_output("profile.html", source, vmstate.profstate);
  
  if (vmstate.runstate == VM_ERRORED) {
    fprintf(stderr, "at:\n");
    vm_print_backtrace(&vmstate);
    fprintf(stderr, "vm failure: %s\n", vmstate.error);
  }
  
  printf("(%i cycles)\n", cyclecount);
  
  gc_remove_roots(&vmstate, &set);
  
  // one last run, deleting everything
  gc_run(&vmstate);
  
  return 0;
}
