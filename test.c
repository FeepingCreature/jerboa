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
    "function ack(m, n) {"
    "   var np = n + 1., nm = n - 1., mm;"
    "   mm = m - 1.;"
    "   if (m < 0.5) return np;"
    "   if (n < 0.5) return ack(mm, 1.);"
    "   return ack(mm, ack(m, nm));"
    "}";
  
  UserFunction *module = parse_module(&text);
  dump_fn(module);
  
  root = call_function(root, module, NULL, 0);
  
  Object *ack = object_lookup(root, "ack");
  
  Object **args_ptr = malloc(sizeof(Object*) * 2);
  args_ptr[0] = alloc_float(root, 3);
  args_ptr[1] = alloc_float(root, 7);
  Object *res = function_handler(root, NULL, ack, args_ptr, 2);
  
  Object *int_base = object_lookup(root, "int");
  if (res->parent == int_base) {
    IntObject *res_int = (IntObject*) res;
    printf("ack(3., 7.) = %i\n", res_int->value);
  } else {
    FloatObject *res_float = (FloatObject*) res;
    printf("ack(3., 7.) = %f\n", res_float->value);
  }
  printf("(%i cycles)\n", cyclecount);
  
  gc_remove_roots(entry);
  gc_run();
  
  return 0;
}
