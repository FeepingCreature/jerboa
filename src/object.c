#include <stdio.h>

#include <fcntl.h>
#include <errno.h>
#include <unistd.h>

#include "object.h"
#include "trie.h"

#define DEBUG_MEM 0

#define FREELIST_LIMIT 32

void *freelist[FREELIST_LIMIT] = {0};

void *cache_alloc_uninitialized(int size) {
  if (UNLIKELY(size == 0)) return NULL;
  size = (size + 15) & ~15; // align to 16
  // printf(": %i\n", size);
  int slot = size >> 4;
  void *res;
  if (UNLIKELY((size == 32 || size == 64 || size == 128) && !freelist[slot])) {
    // feed the freelist with a single big alloc
    void *bigalloc = malloc(size * 1024);
    // stitch backwards
    void *cursor = (char*) bigalloc + size * (1024 - 1);
    void *needle = freelist[slot];
    for (int i = 0; i < 1024; ++i) {
      *(void**) cursor = needle;
      needle = cursor;
      cursor = (char*) cursor - size;
    }
    freelist[slot] = needle;
  }
  if (LIKELY(slot < FREELIST_LIMIT && freelist[slot])) {
    res = freelist[slot];
    freelist[slot] = *(void**) freelist[slot];
  } else {
    // printf(": alloc %i\n", size);
    res = malloc(size);
  }
  return res;
}

void cache_free(int size, void *ptr) {
  if (UNLIKELY(size == 0)) return;
  size = (size + 15) & ~15; // align to 16
  int slot = size >> 4;
  if (slot < FREELIST_LIMIT) {
    *(void**) ptr = freelist[slot];
    freelist[slot] = ptr;
    return;
  }
  free(ptr);
}

void free_cache(VMState *state) {
  fprintf(stderr, "Calling this function is not presently safe, since not every pointer in the freelist points to the start of an object.\n");
  abort();
  for (int i = 0; i < FREELIST_LIMIT; i++) {
    void *ptr = freelist[i];
    while (ptr) {
      void *prev_ptr = ptr;
      ptr = *(void**) ptr;
      free(prev_ptr);
    }
    freelist[i] = NULL;
  }
  if (state->shared->stack_data_offset > 0) {
    fprintf(stderr, "stack nonzero at exit!\n");
    abort();
  }
  free(state->shared->stack_data_ptr);
  state->shared->stack_data_ptr = NULL;
  state->shared->stack_data_len = 0;
}

Value object_lookup_p(Object *obj, FastKey *key, bool *key_found_p) {
  assert(key_found_p && *key_found_p == false);
  do {
    TableEntry *entry = table_lookup_prepared(&obj->tbl, key);
    if (entry) { *key_found_p = true; return entry->value; }
    obj = obj->parent;
  } while (obj);
  return VNULL;
}

Value object_lookup(Object *obj, FastKey *key) {
  assert(obj);
  do {
    TableEntry *entry = table_lookup_prepared(&obj->tbl, key);
    if (entry) return entry->value;
    obj = obj->parent;
  } while (obj);
  return VNULL;
}

Object *closest_obj(VMState *state, Value val) {
  assert(IS_NULL(val) || IS_INT(val) || IS_BOOL(val) || IS_FLOAT(val) || IS_OBJ(val));
  if (IS_OBJ(val)) return AS_OBJ(val);
  return VCACHE_BY_TYPE(state->shared->vcache, val.type);
}

Object *proto_obj(VMState *state, Value val) {
  if (IS_OBJ(val)) return AS_OBJ(val)->parent;
  return VCACHE_BY_TYPE(state->shared->vcache, val.type);
}

void obj_mark(VMState *state, Object *obj) {
  if (!obj) return;
  
  if (obj->flags & OBJ_GC_MARK) return; // break cycles
  
  obj->flags |= OBJ_GC_MARK;
  
  obj_mark(state, obj->parent);
  
  HashTable *tbl = &obj->tbl;
  for (int i = 0; i < tbl->entries_num; ++i) {
    TableEntry *entry = &tbl->entries_ptr[i];
    if (entry->hash && IS_OBJ(entry->value)) {
      obj_mark(state, AS_OBJ(entry->value));
    }
  }
  
  if (obj->mark_fn) {
    obj->mark_fn(state, obj);
  }
}

void obj_free_aux(Object *obj) {
  if (obj->free_fn) {
    obj->free_fn(obj);
  }
  if (!(obj->flags & OBJ_INLINE_TABLE)) {
    cache_free(sizeof(TableEntry) * obj->tbl.entries_num, obj->tbl.entries_ptr);
  }
}

void obj_free(Object *obj) {
  obj_free_aux(obj);
  cache_free(obj->size, obj);
}

Object *obj_instance_of(Object *obj, Object *proto) {
  if (proto == NULL) {
    fprintf(stderr, "vacuous case\n");
    abort();
  }
  while (obj) {
    if (obj->parent == proto) return obj;
    obj = obj->parent;
  }
  return NULL;
}

bool value_instance_of(VMState *state, Value val, Object *proto) {
  if (proto == NULL) {
    fprintf(stderr, "vacuous case\n");
    abort();
  }
  if (IS_NULL(val)) return false;
  Object *obj = proto_obj(state, val);
  while (obj) {
    if (obj == proto) return true;
    obj = obj->parent;
  }
  return false;
}

bool value_is_truthy(Value value) {
  if (LIKELY(IS_BOOL(value))) return AS_BOOL(value);
  else if (IS_NULL(value)) return false;
  else if (IS_INT(value)) return AS_INT(value) != 0;
  else return true; // generic object
}

bool value_fits_constraint(VMSharedState *sstate, Value value, Object *constraint) {
  if (!constraint) return true;
  if (IS_NULL(value)) return false;
  if (IS_INT(value)) return constraint == sstate->vcache.int_base;
  if (IS_BOOL(value)) return constraint == sstate->vcache.bool_base;
  if (IS_FLOAT(value)) return constraint == sstate->vcache.float_base;
  return obj_instance_of(AS_OBJ(value), constraint) != NULL;
}

void value_failed_type_constraint_error(VMState *state, Object *constraint, Value value) {
  VM_ASSERT(false,
            "value failed type constraint: constraint was %s, but value was %s",
            get_type_info(state, OBJ2VAL(constraint)), get_type_info(state, value));
}

// returns error or null
char *object_set_constraint(VMState *state, Object *obj, FastKey *key, Object *constraint) {
  assert(obj != NULL);
  TableEntry *entry = table_lookup_prepared(&obj->tbl, key);
  if (!constraint) return "tried to set constraint that was null";
  if (!entry) return "tried to set constraint on a key that was not yet defined!";
  if (entry->constraint) return "tried to set constraint, but key already had a constraint!";
  Value existing_value = entry->value;
  if (!value_fits_constraint(state->shared, existing_value, constraint)) {
    return my_asprintf("value failed type constraint: constraint was %s, but value was %s",
                       get_type_info(state, OBJ2VAL(constraint)), get_type_info(state, existing_value));
  }
  // There is no need to check flags here - constraints cannot be changed and don't modify existing data.
  entry->constraint = constraint;
  return NULL;
}

// change a property in-place
// returns an error string or NULL
char *object_set_existing(VMState *state, Object *obj, FastKey *key, Value value) {
  assert(obj != NULL);
  Object *current = obj;
  while (current) {
    TableEntry *entry = table_lookup_prepared(&current->tbl, key);
    if (entry != NULL) {
      if (current->flags & OBJ_FROZEN) {
        return my_asprintf("Tried to set existing key '%s', but object %p was frozen.", trie_reverse_lookup(key->hash), (void*) current);
      }
      if (!value_fits_constraint(state->shared, value, entry->constraint)) {
        return "type constraint violated on assignment";
      }
      entry->value = value;
      return NULL;
    }
    current = current->parent;
  }
  return my_asprintf("Key '%s' not found in object %p.", trie_reverse_lookup(key->hash), (void*) obj);
}

// change a property but only if it exists somewhere in the prototype chain
// returns error or null on success
char *object_set_shadowing(VMState *state, Object *obj, FastKey *key, Value value, bool *value_set) {
  assert(obj != NULL);
  Object *current = obj;
  while (current) {
    TableEntry *entry = table_lookup_prepared(&current->tbl, key);
    if (entry) {
      if (!value_fits_constraint(state->shared, value, entry->constraint)) {
        return "type constraint violated on shadowing assignment";
      }
      // so create it in obj (not current!)
      object_set(state, obj, key, value);
      if (current != obj && entry->constraint) {
        // propagate constraint
        char *error = object_set_constraint(state, obj, key, entry->constraint);
        if (error) return error;
      }
      *value_set = true;
      return NULL;
    }
    current = current->parent;
  }
  *value_set = false;
  return NULL;
}

// returns error or null
char *object_set(VMState *state, Object *obj, FastKey *key, Value value) {
  assert(obj != NULL);
  
  // check constraints in parents
  Object *current = obj->parent;
  while (current) {
    TableEntry *entry = table_lookup_prepared(&current->tbl, key);
    if (entry) {
      if (!value_fits_constraint(state->shared, value, entry->constraint)) {
        return "type constraint in parent violated on assignment";
      }
    }
    current = current->parent;
  }
  
  // TODO check flags beforehand to avoid clobbering tables that are frozen
  TableEntry *freeptr;
  TableEntry *entry = table_lookup_alloc_prepared(&obj->tbl, key, &freeptr);
  if (entry) {
    assert(!(obj->flags & OBJ_FROZEN));
    if (!value_fits_constraint(state->shared, value, entry->constraint)) {
      return "type constraint violated on assignment";
    }
    entry->value = value;
  } else {
    assert(!(obj->flags & OBJ_CLOSED));
    freeptr->value = value;
  }
  return NULL;
}

void vm_record_profile(VMState *state);
void *alloc_object_internal(VMState *state, int size, bool stack) {
  Object *res;
  if (stack) {
    res = vm_stack_alloc_uninitialized(state, size);
    if (UNLIKELY(!res)) return NULL;
    *res = (Object) {
#if COUNT_OBJECTS
      .alloc_id = state->shared->gcstate.num_obj_allocated_total++,
#endif
      .prev = state->frame->last_stack_obj,
      .size = size,
    };
    state->frame->last_stack_obj = res;
  } else {
    res = cache_alloc_uninitialized(size);
    *res = (Object) {
#if COUNT_OBJECTS
      .alloc_id = state->shared->gcstate.num_obj_allocated_total++,
#endif
      .prev = state->shared->gcstate.last_obj_allocated,
      .size = size,
    };
    // for debugging
    /*if (res->alloc_id == 535818) {
      __asm__("int $3");
    }*/
    state->shared->gcstate.last_obj_allocated = res;
    state->shared->gcstate.bytes_allocated += size;
  }
  
#if DEBUG_MEM
  fprintf(stderr, "alloc object %p\n", (void*) res);
#endif
  
  // if(state->runstate == VM_RUNNING) vm_record_profile(state);
  
  return res;
}

Value make_object(VMState *state, Object *parent, bool stack) {
  Object *obj = alloc_object_internal(state, sizeof(Object), stack);
  if (!obj) return VNULL; // alloc failed
  obj->parent = parent;
  return OBJ2VAL(obj);
}

Value make_string(VMState *state, const char *ptr, int len) {
  // allocate the string as part of the object, so that it gets freed with the object
  StringObject *obj = alloc_object_internal(state, sizeof(StringObject) + len + 1, false);
  obj->base.parent = state->shared->vcache.string_base;
  // obj->base.flags = OBJ_IMMUTABLE | OBJ_CLOSED;
  obj->static_ptr = true; // value must not be freed; it is included in the object
  obj->value = ((char*) obj) + sizeof(StringObject);
  strncpy(obj->value, ptr, len);
  obj->value[len] = 0;
  return OBJ2VAL((Object*) obj);
}

Value make_string_static(VMState *state, char *value) {
  StringObject *obj = alloc_object_internal(state, sizeof(StringObject), false);
  obj->base.parent = state->shared->vcache.string_base;
  // obj->base.flags = OBJ_IMMUTABLE | OBJ_CLOSED;
  obj->static_ptr = true;
  obj->value = value;
  return OBJ2VAL((Object*) obj);
}

static void array_mark_fn(VMState *state, Object *obj) {
  ArrayObject *arr_obj = (ArrayObject*) obj;
  if (arr_obj) { // else it's obj == array_base
    for (int i = 0; i < arr_obj->length; ++i) {
      Value v = arr_obj->ptr[i];
      if (IS_OBJ(v)) obj_mark(state, AS_OBJ(v));
    }
  }
}

static void array_free_fn(Object *obj) {
  ArrayObject *arr_obj = (ArrayObject*) obj;
  if (arr_obj->owned) {
    free(arr_obj->ptr);
  }
}

Value make_array(VMState *state, Value *ptr, int length, bool owned) {
  ArrayObject *obj = alloc_object_internal(state, sizeof(ArrayObject), false);
  obj->base.parent = state->shared->vcache.array_base;
  obj->base.mark_fn = array_mark_fn;
  obj->base.free_fn = array_free_fn;
  obj->ptr = ptr;
  obj->length = length;
  if (owned) obj->capacity = length;
  else obj->capacity = 0;
  obj->owned = owned;
  OBJECT_SET(state, (Object*) obj, length, INT2VAL(length));
  return OBJ2VAL((Object*) obj);
}

// bit twiddle
static unsigned int next_pow2(unsigned int x) {
  x -= 1; // don't do anything to existing powers of two
  x |= (x >> 1); // fill 1s to the right
  x |= (x >> 2);
  x |= (x >> 4);
  x |= (x >> 8);
  x |= (x >> 16); // until the number is [all 0s] [all 1s]
  return x + 1; // add 1 to get a clean power of two.
}

void array_resize(VMState *state, ArrayObject *aobj, int newsize, bool update_len) {
  assert(aobj->owned);
  if (newsize > aobj->capacity) {
    int newcap = next_pow2(newsize);
    aobj->ptr = realloc(aobj->ptr, sizeof(Value) * newcap);
    aobj->capacity = newcap;
  }
  aobj->length = newsize;
  if (update_len) OBJECT_SET(state, (Object*) aobj, length, INT2VAL(aobj->length));
}

Value make_ptr(VMState *state, void *ptr) {
  PointerObject *obj = alloc_object_internal(state, sizeof(PointerObject), false);
  obj->base.parent = state->shared->vcache.pointer_base;
  obj->base.flags = OBJ_NOINHERIT;
  obj->ptr = ptr;
  return OBJ2VAL((Object*) obj);
}

// used to allocate "special functions" like ffi functions, that need to store more data
Value make_fn_custom(VMState *state, VMFunctionPointer fn, InstrDispatchFn dispatch_fn, int size_custom) {
  assert(size_custom >= sizeof(FunctionObject));
  FunctionObject *obj = alloc_object_internal(state, size_custom, false);
  obj->base.parent = state->shared->vcache.function_base;
  obj->base.flags |= OBJ_NOINHERIT;
  obj->fn_ptr = fn;
  obj->dispatch_fn_ptr = dispatch_fn;
  return OBJ2VAL((Object*) obj);
}

Value make_fn(VMState *state, VMFunctionPointer fn) {
  return make_fn_custom(state, fn, NULL, sizeof(FunctionObject));
}

Value make_fn_fast(VMState *state, VMFunctionPointer fn, InstrDispatchFn dispatch_fn) {
  return make_fn_custom(state, fn, dispatch_fn, sizeof(FunctionObject));
}

char *get_val_info(VMState *state, Value val) {
  if (IS_NULL(val)) return "<null>";
  else if (IS_INT(val)) return my_asprintf("<int: %i>", AS_INT(val));
  else if (IS_BOOL(val)) return my_asprintf("<bool: %s>", AS_BOOL(val)?"true":"false");
  else if (IS_FLOAT(val)) return my_asprintf("<float: %f>", AS_FLOAT(val));
  else {
    if (state) {
      StringObject *sobj = (StringObject*) obj_instance_of(AS_OBJ(val), state->shared->vcache.string_base);
      if (sobj) {
        return my_asprintf("<obj:string: '%s'>", sobj->value);
      }
    }
    return my_asprintf("<obj: %p>", (void*) AS_OBJ(val));
  }
}

void free_function(UserFunction *uf) {
  free(uf->body.blocks_ptr);
  free(uf->body.instrs_ptr);
  free(uf->body.ranges_ptr);
  free(uf);
}

// TODO move elsewhere

typedef struct {
  FileRange *range;
  HashTable *table; // indirect
  int num_samples; // direct
} ProfilerRecord;

int prec_sort_outside_in_fn(const void *a, const void *b) {
  const ProfilerRecord *rec_a = a, *rec_b = b;
  // ranges that start earlier
  if (rec_a->range->text_from < rec_b->range->text_from) return -1;
  if (rec_a->range->text_from > rec_b->range->text_from) return 1;
  // then ranges that end later
  if (rec_a->range->text_len > rec_b->range->text_len) return -1;
  if (rec_a->range->text_len < rec_b->range->text_len) return 1;
  // otherwise they're identical??
  return 0;
}

// TODO move into separate file
void save_profile_output(char *filename, VMProfileState *profile_state) {
  FILE *file = fopen(filename, "w");
  if (file == NULL) {
    fprintf(stderr, "error creating profiler output file: %s\n", strerror(errno));
    abort();
  }
  
  HashTable *excl_table = &profile_state->excl_table;
  HashTable *incl_table = &profile_state->incl_table;
  int num_excl_records = excl_table->entries_stored;
  int num_incl_records = incl_table->entries_stored;
  int num_records = num_excl_records + num_incl_records;
  
  ProfilerRecord *record_entries = calloc(sizeof(ProfilerRecord), num_records);
  
  int k = 0;
  for (int i = 0; i < excl_table->entries_num; ++i) {
    TableEntry *entry = &excl_table->entries_ptr[i];
    if (entry->hash) {
      // TODO encode as offset so we can change hash to 32-bit
      FileRange *range = (FileRange*) entry->hash;
      int samples = entry->value.i;
      // printf("dir entry %i of %i: %p; %i %.*s (%i)\n", i, excl_table->entries_num, (void*) range, (int) range->text_len, (int) range->text_len, range->text_from, samples);
      record_entries[k++] = (ProfilerRecord) { .range = range, .table = NULL, .num_samples = samples };
    }
  }
  for (int i = 0; i < incl_table->entries_num; ++i) {
    TableEntry *entry = &incl_table->entries_ptr[i];
    if (entry->hash) {
      FileRange *range = (FileRange*) entry->hash;
      HashTable *table = (HashTable*) entry->value.obj;
      // printf("indir entry %i of %i: %i %.*s (%i)\n", i, incl_table->entries_num, (int) range->text_len, (int) range->text_len, range->text_from, table->entries_num);
      record_entries[k++] = (ProfilerRecord) { .range = range, .table = table, .num_samples = 0 };
    }
  }
  assert(k == num_records);
  
  qsort(record_entries, num_records, sizeof(ProfilerRecord), prec_sort_outside_in_fn);
  
#define CUR_ENTRY (&record_entries[cur_entry_id])
  
  fprintf(file, "events: Samples\n\n");
  
  int num_files = 0;
  FileEntry *files = get_files(&num_files);
  for (int i = 0; i < num_files; i++) {
    fprintf(file, "fl=%s\n", files[i].file);
    TextRange source = files[i].range;
    
    int cur_entry_id = 0;
    // skip any preceding
    while (cur_entry_id < num_records && CUR_ENTRY->range->text_from < source.start) {
      cur_entry_id ++;
    }
    
    int last_row = -1;
    int row_samples = 0;
    
    const char *last_fn = "placeholder";
    
    HashTable row_calls = {0};
    
    for (; cur_entry_id < num_records && CUR_ENTRY->range->text_from < source.end; cur_entry_id ++) {
      int num_samples = CUR_ENTRY->num_samples;
      const char *name, *fn; TextRange line; int row, col;
      if (!find_text_pos(CUR_ENTRY->range->text_from, &name, &fn, &line, &row, &col)) continue;
      // fprintf(stderr, "for %p '%.*s': %s, %s, %i, %i: %i\n", (void*) CUR_ENTRY->range, CUR_ENTRY->range->text_len, CUR_ENTRY->range->text_from, name, fn, row, col, num_samples);
      // if we need to flush / we're in the last loop
      if (row != last_row || cur_entry_id >= num_records - 1 || record_entries[cur_entry_id+1].range->text_from >= source.end) {
        if (last_row != -1) {
          fprintf(file, "%i %i\n", last_row+1, row_samples);
          // fprintf(stderr, ": %i %i\n", last_row+1, row_samples);
          // fprintf(stderr, ": %i calls\n", row_calls.entries_num);
          for (int l = 0; l < row_calls.entries_num; l++) {
            TableEntry *entry = &row_calls.entries_ptr[l];
            if (entry->hash) {
              FileRange *fun_range = (FileRange*) entry->hash;
              int samples = entry->value.i;
              const char *name2, *fn2; TextRange line2; int row2, col2;
              // find call target site
              if (!find_text_pos(fun_range->text_from, &name2, &fn2, &line2, &row2, &col2)) continue;
              if (name != name2) fprintf(file, "cfi=%s\n", name2);
              fprintf(file, "cfn=%s\n", fn2);
              fprintf(file, "calls=1 %i\n", row2+1);
              fprintf(file, "%i %i\n", last_row+1, samples);
            }
          }
        }
        if (fn != last_fn) {
          last_fn = fn;
          if (fn) fprintf(file, "fn=%s\n", fn);
          else fprintf(file, "fn=Unknown\n");
        }
        last_row = row;
        row_samples = 0;
        table_free(&row_calls);
        row_calls = (HashTable) {0};
      }
      row_samples += num_samples;
      if (CUR_ENTRY->table) {
        HashTable *sub_table = CUR_ENTRY->table;
        for (int l = 0; l < sub_table->entries_num; l++) {
          TableEntry *entry = &sub_table->entries_ptr[l];
          if (entry->hash) {
            // function range
            FastKey key = { .hash = entry->hash };
            TableEntry *freeptr;
            TableEntry *call_p = table_lookup_alloc_prepared(&row_calls, &key, &freeptr);
            if (freeptr) freeptr->value.i = entry->value.i;
            else call_p->value.i += entry->value.i;
          }
        }
      }
    }
  }
#undef CUR_ENTRY
  
  fclose(file);
}
