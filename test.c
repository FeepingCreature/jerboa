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

int main(int argc, char **argv) {
  Object *root = create_root();
  
  void *entry = gc_add_roots(&root, 1);
  
  char *text =
    "var obj = {a: 5, b: null, bar: method() { print(this.a - this.b); } };"
    "obj.b = 7;"
    "var obj2 = new obj { b: 9 };"
    "var objx = new 5 { bar: 7 }; print(\"objx = \"+(objx + objx.bar));"
    "obj[\"foo\"] = method() { print(this.a + this.b); };"
    "obj.foo();"
    "obj.bar();"
    "obj2.foo();"
    "obj2.bar();"
    "var Class = { a: 0 };"
    "var SubClass = new Class { b: 0, test: method() { print(\"a + b = \" + (this.a + this.b)); } };"
    "var obj = new SubClass;"
    "obj.a = 5;"
    "obj.b = 8;"
    "obj.test();"
    "print(\"debug: \"+obj.a+\", \"+obj.b);"
    "print(\"2 != 2 = \"+(2 != 2));"
    "print(\"2 !< 2 = \"+(2 !< 2));"
    "print(\"2 !> 2 = \"+(2 !> 2));"
    "print(\"2 !<= 2 = \"+(2 !<= 2));"
    "print(\"2 !>= 2 = \"+(2 !>= 2));"
    "var i = 0; while (i < 10) { print(\"i = \"+i); i = i + 1; }"
    "function ack(m, n) {"
    "  var np = n + 1, nm = n - 1, mm;"
    "  mm = m - 1;"
    "  if (m < 0.5) return np;"
    "  if (n == 0) return ack(mm, 1);"
    "  return ack(mm, ack(m, nm));"
    "}"
    "print(\"ack(3, 7) = \"+ack(3, 7));"
    ;
  
  UserFunction *module = parse_module(&text);
  dump_fn(module);
  
  root = call_function(root, module, NULL, 0);
  
  printf("(%i cycles)\n", cyclecount);
  
  gc_remove_roots(entry);
  
  // one last run, deleting everything
  gc_run(root);
  
  return 0;
}
