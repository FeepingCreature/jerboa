#ifndef JERBOA_CORE_H
#define JERBOA_CORE_H

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <stdbool.h>

#include "win32_compat.h"

// core definitions (factored out because they're all super circular)

// **IMPORTANT**
// non-object values act like objects OBJ_CLOSED, OBJ_FROZEN, OBJ_NOINHERIT

typedef struct _TableEntry TableEntry;

typedef struct _HashTable HashTable;

typedef struct _VMState VMState;

typedef struct _Object Object;

typedef struct _GCRootSet GCRootSet;

typedef struct _Callframe Callframe;

typedef enum {
  INSTR_INVALID = -1,
  INSTR_ALLOC_OBJECT,
  INSTR_ALLOC_INT_OBJECT,
  INSTR_ALLOC_BOOL_OBJECT,
  INSTR_ALLOC_FLOAT_OBJECT,
  INSTR_ALLOC_ARRAY_OBJECT,
  INSTR_ALLOC_STRING_OBJECT,
  INSTR_ALLOC_CLOSURE_OBJECT,
  INSTR_FREE_OBJECT,
  INSTR_CLOSE_OBJECT,
  INSTR_FREEZE_OBJECT,
  INSTR_ACCESS,
  INSTR_ASSIGN,
  INSTR_KEY_IN_OBJ,
  INSTR_IDENTICAL,
  INSTR_INSTANCEOF,
  INSTR_SET_CONSTRAINT,
  INSTR_CALL,
  INSTR_TEST, // turns an arbitrary value into a bool (truthiness)
  // Note: block-ending instructions must not access refslots!
  // this is so we can insert stack-cleanup commands before them
  INSTR_RETURN,
  INSTR_BR,
  INSTR_TESTBR,
  INSTR_PHI,
  // everything below this point is only generated by optimizer passes
  INSTR_ACCESS_STRING_KEY,
  INSTR_ASSIGN_STRING_KEY,
  INSTR_STRING_KEY_IN_OBJ,
  INSTR_SET_CONSTRAINT_STRING_KEY,
  INSTR_DEFINE_REFSLOT,
  INSTR_MOVE,
  INSTR_CALL_FUNCTION_DIRECT,
  // object is allocated, some fields are defined, object is closed, and refslots are created for its fields
  // this is a very common pattern due to scopes
  INSTR_ALLOC_STATIC_OBJECT,
  
  INSTR_LAST
} InstrType;

#define UNLIKELY(X) __builtin_expect(!!(X), 0)
#define LIKELY(X) __builtin_expect(!!(X), 1)

struct _HashTable {
  TableEntry *entries_ptr;
  int entries_num;
  int entries_stored;
  size_t bloom;
};

typedef enum {
  OBJ_NONE = 0,
  OBJ_CLOSED = 0x1, // no entries can be added or removed
  OBJ_FROZEN = 0x2, // no entries' values can be changed, no entries can be removed
  OBJ_NOINHERIT = 0x4, // don't allow the user to use this as a prototype
                       // used for prototypes of objects with payload,
                       // like array or function, that have their own alloc functions.
                       // you can still prototype the objects themselves though.
  OBJ_GC_MARK = 0x8,   // reachable in the "gc mark" phase
  OBJ_IMMORTAL = 0x10, // will never be freed
  OBJ_INLINE_TABLE = 0x20, // table is allocated with the object, doesn't need to be freed separately
  OBJ_PRINT_HACK = 0x40, // lol
  OBJ_STACK_FREED = 0x80, // stack allocated object is marked freed, and will be
                          // cleaned up once the allocations on top of it are gone
} ObjectFlags;

// for debugging specific objects
#define COUNT_OBJECTS 0

struct _Object {
  Object *parent;
  int size;
  ObjectFlags flags;
  Object *prev; // for gc
#if COUNT_OBJECTS
  int alloc_id;
#endif
  
  HashTable tbl;
  void (*mark_fn)(VMState *state, Object *obj); // for gc
  void (*free_fn)(Object *obj); // for gc
};

typedef enum {
  // DO NOT CHANGE ORDER (see object.c: closest_obj)
  TYPE_NULL = 0, // all object references must be non-null
  TYPE_INT = 1,
  TYPE_FLOAT = 2,
  TYPE_BOOL = 3,
  TYPE_OBJECT = 4
} TypeTag;

typedef struct {
  long unsigned int type;
  union {
    int i;
    float f;
    bool b;
    Object *obj;
  };
} Value;

typedef enum {
  // DO NOT CHANGE ORDER (see object.h:load_arg)
  ARG_SLOT = 0,
  ARG_REFSLOT = 1,
  ARG_VALUE = 2,
  ARG_POINTER = ARG_VALUE // for WriteArg
} ArgKind;

typedef struct {
  ArgKind kind;
  union {
    int slot;
    int refslot;
    Value value;
  };
} Arg;

typedef struct {
  ArgKind kind;
  union {
    int slot;
    int refslot;
    Value *pointer;
  };
} WriteArg;

char *get_write_arg_info(WriteArg warg);
char *get_arg_info(VMState *state, Arg arg);

typedef struct {
  Arg fn;
  Arg this_arg;
  WriteArg target; // duplicated here
  int args_len;
} CallInfo;

#define INFO_ARGS_PTR(I) ((Arg*)(I + 1))

// key, interned and prehashed for fast lookup
typedef struct {
  size_t len;
  const char *ptr;
  size_t hash;
  size_t last_index; // instrs usually access the same object, or objects with the same layout
} FastKey;

struct _TableEntry {
  const char *key_ptr;
  Object *constraint;
  Value value;
};

// TODO actually use
#define TBL_GRAVESTONE = ((const char*) -1);

#define IS_NULL(V) ((V).type == TYPE_NULL)

#define NOT_NULL(V) ((V).type != TYPE_NULL)

#define VNULL ((Value) { .type = TYPE_NULL, .obj = NULL })

static inline int as_int_(Value v) { assert(v.type == TYPE_INT); return v.i; }
static inline bool as_bool_(Value v) { assert(v.type == TYPE_BOOL); return v.b; }
static inline float as_float_(Value v) { assert(v.type == TYPE_FLOAT); return v.f; }
static inline Object *as_obj_(Value v) { assert(v.type == TYPE_OBJECT); return v.obj; }
static inline Object *obj_or_null_(Value v) { Object *sel[] = { NULL, v.obj }; return sel[v.type == TYPE_OBJECT]; }

#define IS_INT(V) ((V).type == TYPE_INT)
#define IS_BOOL(V) ((V).type == TYPE_BOOL)
#define IS_FLOAT(V) ((V).type == TYPE_FLOAT)
#define IS_OBJ(V) ((V).type == TYPE_OBJECT)

#ifndef NDEBUG
#define AS_INT(V) (as_int_(V))
#define AS_BOOL(V) (as_bool_(V))
#define AS_FLOAT(V) (as_float_(V))
#define AS_OBJ(V) (as_obj_(V))
#else
#define AS_INT(V) ((V).i)
#define AS_BOOL(V) ((V).b)
#define AS_FLOAT(V) ((V).f)
#define AS_OBJ(V) ((V).obj)
#endif
#define OBJ_OR_NULL(V) (obj_or_null_(V))

#define INT2VAL(I) ((Value) { .type = TYPE_INT, .i = (I) })
#define BOOL2VAL(B) ((Value) { .type = TYPE_BOOL, .b = (B) })
#define FLOAT2VAL(F) ((Value) { .type = TYPE_FLOAT, .f = (F) })

#ifndef NDEBUG
static inline Value obj2val_checked(Object *obj) {
  assert(obj != NULL);
  return (Value) { .type = TYPE_OBJECT, .obj = obj };
}
#define OBJ2VAL(O) obj2val_checked(O)
#else
#define OBJ2VAL(O) ((Value) { .type = TYPE_OBJECT, .obj = (O) })
#endif

typedef enum {
  VM_RUNNING,
  VM_TERMINATED,
  VM_ERRORED,
} VMRunState;

typedef struct {
  struct timespec last_prof_time;
  bool profiling_enabled;
  
  HashTable direct_table;
  HashTable indirect_table;
} VMProfileState;

typedef struct {
  Object *int_base, *bool_base, *float_base;
  Object *closure_base, *function_base;
  Object *array_base, *string_base, *pointer_base;
  Object *ffi_obj; // cached here so ffi_call_fn can be fast
  FastKey thiskey;
} ValueCache;

struct _GCRootSet {
  Value *values;
  int num_values;
  GCRootSet *prev, *next;
};

typedef struct {
  GCRootSet head, tail; // always empty; using values to anchor lets us avoid branches in gc functions
  
  Object *last_obj_allocated;
  int bytes_allocated, next_gc_run;
#if COUNT_OBJECTS
  int num_obj_allocated_total;
#endif
  
  int disabledness;
  bool missed_gc; // tried to run gc when it was disabled
} GCState;

// shared between parent and child VMs
typedef struct {
  GCState gcstate;
  VMProfileState profstate;
  ValueCache vcache;
  int cyclecount;
  
  // backing storage for stack allocations
  // cannot be moved after the fact!
  // stored in shared_state because sub-vms stick to callstack order
  void *stack_data_ptr; int stack_data_len;
  int stack_data_offset;
  
  bool verbose;
} VMSharedState;

typedef struct {
  char *text_from;
  int text_len;
  int last_cycle_seen;
} FileRange;

struct _FnWrap;
typedef struct _FnWrap FnWrap;

#ifdef _WIN32
#define FAST_DECL
#else
#define FAST_DECL __attribute__ ((regparm (3)))
#endif
#define FAST_FN __attribute__ ((hot)) FAST_DECL

typedef FnWrap (*VMInstrFn)(VMState *state) FAST_DECL;
struct _FnWrap {
  VMInstrFn self;
};

// "halt execution" marker
// (doesn't need to be fast)
FnWrap vm_halt(VMState *state) FAST_DECL;

typedef struct {
  union {
    void *filler; // Instr must be at least (void*) sized
    struct {
      VMInstrFn fn; // cache
      InstrType type;
      int context_slot; // this is actually important - it keeps the stackframe
                        // alive in the optimizer (CAREFUL when removing)
    };
  };
} Instr;

typedef struct {
  int offset, size;
} InstrBlock;

typedef struct {
  InstrBlock* blocks_ptr; int blocks_len;
  FileRange **ranges_ptr; // indexed by "(char*)current instr - (char*) first instr", on the premise that Instr is at least void*
  // first instruction of first block to last instruction of last block
  // (linear because cache)
  Instr *instrs_ptr, *instrs_ptr_end;
} FunctionBody;

FileRange **instr_belongs_to_p(FunctionBody *body, Instr *instr);

typedef struct {
  int arity; // first n slots are reserved for parameters
  int slots, refslots;
  char *name;
  bool is_method, variadic_tail;
  FunctionBody body;
  bool non_ssa, optimized, resolved;
  int num_optimized;
} UserFunction;

void free_function(UserFunction *uf);

struct _Callframe {
  UserFunction *uf;
  Value *slots_ptr; int slots_len;
  TableEntry **refslots_ptr; int refslots_len; // references to values in closed objects
  GCRootSet frameroot_slots; // gc entries
  // this is set from state->instr as the callframe becomes "not the top frame"
  // if a callframe is the top frame, you should always be using state->instr.
  Instr *instr_ptr;
  // set on user function call, to avoid having to compute it in vm_instr_return
  Instr *return_next_instr;
  // when returning *from* this frame, assign result value to this (in the *above* frame)
  WriteArg target;
  int block, prev_block; // required for phi nodes
  Object *last_stack_obj; // chain of stack allocated objects for freeing later
  Callframe *above;
};

typedef void (*VMFunctionPointer)(VMState *state, CallInfo *info);

static inline bool values_identical(Value arg1, Value arg2) {
  if (arg1.type != arg2.type) return false;
  else if (arg1.type == TYPE_NULL) return true;
  else if (arg1.type == TYPE_OBJECT) {
    return arg1.obj == arg2.obj;
  } else if (arg1.type == TYPE_BOOL) {
    return arg1.b == arg2.b;
  } else if (arg1.type == TYPE_INT) {
    return arg1.i == arg2.i;
  } else if (arg1.type == TYPE_FLOAT) {
    return arg1.f == arg2.f;
  } else abort();
}

#endif
