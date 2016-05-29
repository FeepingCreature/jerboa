#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <assert.h>

#include "object.h"
#include "vm/call.h"
#include "vm/runtime.h"
#include "vm/dump.h"
#include "parser.h"

int main(int argc, char **argv) {
  Object *root = create_root();
  
  char *text =
    "function ack(m, n) {"
    "   var np = n + 1, nm = n - 1, mm;"
    "   mm = m - 1;"
    "   if (m == 0) return np;"
    "   if (n == 0) return ack(mm, 1);"
    "   return ack(mm, ack(m, nm));"
    "}";
  
  UserFunction *module = parse_module(&text);
  dump_fn(module);
  
  root = call_function(root, module, NULL, 0);
  
  Object *ack = object_lookup(root, "ack");
  
  Object **args_ptr = malloc(sizeof(Object*) * 2);
  args_ptr[0] = alloc_int(root, 3);
  args_ptr[1] = alloc_int(root, 7);
  Object *res = closure_handler(root, ack, args_ptr, 2);
  IntObject *res_int = (IntObject*) res;
  printf("ack(3, 7) = %i\n", res_int->value);
  printf("(%i cycles)\n", cyclecount);
  return 0;
}
