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
  VMState vmstate = {0};
  vm_alloc_frame(&vmstate);
  
  Object *root = create_root(&vmstate);
  
  GCRootSet set;
  gc_add_roots(&vmstate, &root, 1, &set);
  
  String source = readfile("test.jb");
  
  UserFunction *module;
  ParseResult res = parse_module(&source.ptr, &module);
  assert(res == PARSE_OK);
  dump_fn(module);
  
  vmstate.stack_len = 0;
  call_function(&vmstate, root, module, NULL, 0);
  vm_run(&vmstate, root);
  
  printf("(%i cycles)\n", cyclecount);
  
  gc_remove_roots(&vmstate, &set);
  
  // one last run, deleting everything
  gc_run(&vmstate);
  
  return 0;
}
