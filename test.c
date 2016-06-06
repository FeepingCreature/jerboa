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
#include "parser.h"

int main(int argc, char **argv) {
  Object *root = create_root();
  
  void *entry = gc_add_roots(&root, 1);
  
  char *text =
    "var obj = {a: 5};"
    "function ack(m, n) {"
    "   var np = n + 1, nm = n - 1, mm;"
    "   mm = m - 1;"
    "   if (m == 0) return np;"
    "   if (n == 0) return ack(mm, 1);"
    "   return ack(mm, ack(m, nm));"
    "}"
    // "print(\"ack(3, 7) = \"+ack(3, 7));";
    "print(\"ack(3, 7) = \"+ack(3, 7));";
  
  UserFunction *module = parse_module(&text);
  dump_fn(module);
  
  root = call_function(root, module, NULL, 0);
  
  printf("(%i cycles)\n", cyclecount);
  
  gc_remove_roots(entry);
  gc_run();
  
  return 0;
}
