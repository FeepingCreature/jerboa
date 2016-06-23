#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <stdbool.h>
#include <assert.h>
#include <unistd.h>
#include <errno.h>

#include "object.h"
#include "gc.h"
#include "vm/call.h"
#include "vm/runtime.h"
#include "vm/dump.h"
#include "language.h"
#include "util.h"

int main(int argc, char **argv) {
  VMState vmstate = {0};
  vmstate.shared = calloc(sizeof(VMSharedState), 1);
  vm_alloc_frame(&vmstate, 0);
  Object *root = create_root(&vmstate);
  vm_remove_frame(&vmstate);
  vmstate.root = root;
  
  GCRootSet set;
  gc_add_roots(&vmstate, &root, 1, &set);
  
  if (!isatty(1)) { fprintf(stderr, "repl must be running in a terminal!\n"); return 1; }
  while (true) {
    char *line = NULL;
    size_t length = 0;
    printf("> ");
    if (getline(&line, &length, stdin) == -1) {
      fprintf(stderr, "error reading line: %s\n", strerror(errno));
      assert(false);
    }
    UserFunction *line_fn;
    ParseResult res = parse_module(&line, &line_fn);
    if (res == PARSE_ERROR) continue;
    assert(res == PARSE_OK);
    dump_fn(line_fn);
    call_function(&vmstate, root, line_fn, NULL, 0);
    vm_run(&vmstate);
    if (vmstate.runstate == VM_ERRORED) {
      fprintf(stderr, "vm errored: %s\n", vmstate.error);
    } else {
      root = vmstate.result_value;
    }
  }
  
  gc_remove_roots(&vmstate, &set);
  
  gc_run(&vmstate);
  
  return 0;
}
