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
#include "vm/vm.h"
#include "language.h"
#include "util.h"

#ifdef _WIN32
#include <readline/readline.h>
#endif

int main(int argc, char **argv) {
  init_instr_fn_table();
  
  VMState vmstate = {0};
  vmstate.runstate = VM_TERMINATED;
  vmstate.shared = calloc(sizeof(VMSharedState), 1);
  vmstate.shared->stack_data_len = 16*1024*1024;
  vmstate.shared->stack_data_ptr = malloc(vmstate.shared->stack_data_len);
  
  vm_alloc_frame(&vmstate, 0, 0);
  Object *root = create_root(&vmstate);
  Value rootval = OBJ2VAL(root);
  vm_remove_frame(&vmstate);
  vmstate.root = root;
  
  GCRootSet set;
  gc_add_roots(&vmstate, &rootval, 1, &set);
  
  if (!isatty(1)) { fprintf(stderr, "repl must be running in a terminal!\n"); return 1; }
  while (true) {
    char *line = NULL;
#ifdef _WIN32
    line = readline("> ");
#else
    size_t length = 0;
    printf("> ");
    if (getline(&line, &length, stdin) == -1) {
      fprintf(stderr, "error reading line: %s\n", strerror(errno));
      abort();
    }
#endif
    UserFunction *line_fn;
    ParseResult res = parse_module(&line, &line_fn);
    if (res == PARSE_ERROR) continue;
    assert(res == PARSE_OK);
    dump_fn(&vmstate, line_fn);
    CallInfo null_call = {0};
    Value rootval;
    null_call.target = (WriteArg) { .kind = ARG_POINTER, .pointer = &rootval };
    call_function(&vmstate, root, line_fn, &null_call);
    vm_update_frame(&vmstate);
    vm_run(&vmstate);
    if (vmstate.runstate == VM_ERRORED) {
      fprintf(stderr, "vm errored: %s\n", vmstate.error);
    } else {
      root = AS_OBJ(rootval);
    }
  }
  
  gc_remove_roots(&vmstate, &set);
  
  gc_run(&vmstate);
  
  return 0;
}
