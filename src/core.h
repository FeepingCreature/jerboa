#ifndef CORE_H
#define CORE_H

#include <stdlib.h>
#include <assert.h>
#include <stdbool.h>

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
  INSTR_GET_ROOT = 0,
  INSTR_ALLOC_OBJECT,
  INSTR_ALLOC_INT_OBJECT,
  INSTR_ALLOC_BOOL_OBJECT,
  INSTR_ALLOC_FLOAT_OBJECT,
  INSTR_ALLOC_ARRAY_OBJECT,
  INSTR_ALLOC_STRING_OBJECT,
  INSTR_ALLOC_CLOSURE_OBJECT,
  INSTR_CLOSE_OBJECT,
  INSTR_FREEZE_OBJECT,
  INSTR_ACCESS,
  INSTR_ASSIGN,
  INSTR_KEY_IN_OBJ,
  INSTR_INSTANCEOF,
  INSTR_SET_CONSTRAINT,
  INSTR_CALL,
  INSTR_RETURN,
  INSTR_BR,
  INSTR_TESTBR,
  INSTR_PHI,
  // everything below this point is only generated by optimizer passes
  INSTR_ACCESS_STRING_KEY,
  INSTR_ASSIGN_STRING_KEY,
  INSTR_SET_CONSTRAINT_STRING_KEY,
  INSTR_SET_SLOT,
  INSTR_DEFINE_REFSLOT,
  INSTR_READ_REFSLOT,
  INSTR_WRITE_REFSLOT,
  // object is allocated, some fields are defined, object is closed, and refslots are created for its fields
  // this is a very common pattern due to scopes
  INSTR_ALLOC_STATIC_OBJECT,
  
  INSTR_LAST
} InstrType;

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
};

typedef enum {
  TYPE_NULL, // all object references must be non-null
  TYPE_INT,
  TYPE_FLOAT,
  TYPE_BOOL,
  TYPE_OBJECT
} TypeTag;

typedef struct {
  TypeTag type;
  union {
    int i;
    float f;
    bool b;
    Object *obj;
  };
} /*__attribute__((aligned (16)))*/ Value; // TODO

typedef enum {
  ARG_SLOT,
  ARG_REFSLOT,
  ARG_VALUE
} ArgKind;

typedef struct {
  ArgKind kind;
  union {
    int slot;
    int refslot;
    Value value;
  };
} Arg;

char *get_arg_info(Arg arg);

typedef struct {
  Arg fn;
  Arg this_arg;
  int args_len;
} CallInfo;

#define INFO_ARGS_PTR(I) ((Arg*)(I + 1))

struct _TableEntry {
  const char *name_ptr;
  size_t name_len;
  Value value;
  Object *constraint;
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
static inline Object *obj_or_null_(Value v) { if (v.type == TYPE_OBJECT) return v.obj; return NULL; }

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
#define OBJ2VAL(O) ((Value) { .type = TYPE_OBJECT, .obj = (O) })

typedef enum {
  VM_TERMINATED,
  VM_RUNNING,
  VM_ERRORED
} VMRunState;

typedef struct {
  struct timespec last_prof_time;
  
  HashTable direct_table;
  HashTable indirect_table;
} VMProfileState;

typedef struct {
  Object *int_base, *bool_base, *float_base;
  Object *closure_base, *function_base;
  Object *array_base, *string_base, *pointer_base;
  Object *ffi_obj; // cached here so ffi_call_fn can be fast
} ValueCache;

struct _GCRootSet {
  Value *values;
  int num_values;
  GCRootSet *prev, *next;
};

typedef struct {
  GCRootSet *tail;
  
  Object *last_obj_allocated;
  int num_obj_allocated, next_gc_run;
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
  char *file;
  char *text_from; int row_from, col_from;
  char *text_to;   int row_to  , col_to  ;
  int last_cycle_seen;
} FileRange;

typedef struct {
  InstrType type;
  int context_slot;
  FileRange *belongs_to;
} Instr;

// Note: the IR is *lexically ordered*.
// That means, any use of a slot must come after its initialization *in iteration order*.
typedef struct {
  int offset, size;
} InstrBlock;

typedef struct {
  InstrBlock* blocks_ptr; int blocks_len;
  // first instruction of first block to last instruction of last block
  // (linear because cache)
  Instr *instrs_ptr, *instrs_ptr_end;
} FunctionBody;

typedef struct {
  int arity; // first n slots are reserved for parameters
  int slots, refslots;
  char *name;
  bool is_method, variadic_tail;
  FunctionBody body;
  bool non_ssa, optimized;
} UserFunction;

struct _Callframe {
  UserFunction *uf;
  Value *slots_ptr; int slots_len;
  Value **refslots_ptr; int refslots_len; // references to values in closed objects
  GCRootSet frameroot_slots; // gc entries
  Instr *instr_ptr;
  // overrides instr_ptr->belongs_to, used when in a call
  // double pointer due to Dark Magic
  FileRange **backtrace_belongs_to_p;
  Value *target_slot; // when returning to this frame, assign result value to this slot
  int block, prev_block; // required for phi nodes
  Callframe *above;
};

static inline Value load_arg(Callframe *frame, Arg arg) {
  if (arg.kind == ARG_SLOT) {
    assert(arg.slot < frame->slots_len);
    return frame->slots_ptr[arg.slot];
  }
  if (arg.kind == ARG_REFSLOT) {
    assert(arg.slot < frame->refslots_len);
    return *frame->refslots_ptr[arg.refslot];
  }
  assert(arg.kind == ARG_VALUE);
  return arg.value;
}

struct _VMState {
  Callframe *frame;
  Instr *instr;
  
  VMSharedState *shared;
  
  VMRunState runstate;
  
  Object *root;
  Value exit_value; // set when the last stackframe returns
  
  char *error;
  char *backtrace; int backtrace_depth;
  VMState *parent;
};

#endif
