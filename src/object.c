#include <stdio.h>

#include <fcntl.h>
#include <errno.h>
#include <unistd.h>

#include "object.h"

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

Value object_lookup(Object *obj, FastKey *key, bool *key_found_p) {
  if (!key_found_p) {
    while (obj) {
      TableEntry *entry = table_lookup_prepared(&obj->tbl, key);
      if (entry) return entry->value;
      obj = obj->parent;
    }
    return VNULL;
  }
  while (obj) {
    TableEntry *entry = table_lookup_prepared(&obj->tbl, key);
    if (entry) { *key_found_p = true; return entry->value; }
    obj = obj->parent;
  }
  *key_found_p = false;
  return VNULL;
}

Object *closest_obj(VMState *state, Value val) {
  assert(IS_NULL(val) || IS_INT(val) || IS_BOOL(val) || IS_FLOAT(val) || IS_OBJ(val));
  Object *options[] = {
    NULL,
    state->shared->vcache.int_base,
    state->shared->vcache.float_base,
    state->shared->vcache.bool_base,
    val.obj
  };
  return options[val.type];
}

Object *proto_obj(VMState *state, Value val) {
  if (LIKELY(IS_OBJ(val))) return AS_OBJ(val)->parent;
  Object *options[] = {
    NULL,
    state->shared->vcache.int_base,
    state->shared->vcache.float_base,
    state->shared->vcache.bool_base
  };
  return options[val.type];
}

void obj_mark(VMState *state, Object *obj) {
  if (!obj) return;
  
  if (obj->flags & OBJ_GC_MARK) return; // break cycles
  
  obj->flags |= OBJ_GC_MARK;
  
  obj_mark(state, obj->parent);
  
  HashTable *tbl = &obj->tbl;
  for (int i = 0; i < tbl->entries_num; ++i) {
    TableEntry *entry = &tbl->entries_ptr[i];
    if (entry->key_ptr && IS_OBJ(entry->value)) {
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
  if (IS_NULL(value)) return false;
  if (IS_BOOL(value)) return AS_BOOL(value);
  if (IS_INT(value)) return AS_INT(value) != 0;
  return true;
}

bool value_fits_constraint(VMSharedState *sstate, Value value, Object *constraint) {
  if (!constraint) return true;
  if (IS_NULL(value)) return false;
  if (IS_INT(value)) return constraint == sstate->vcache.int_base;
  if (IS_BOOL(value)) return constraint == sstate->vcache.bool_base;
  if (IS_FLOAT(value)) return constraint == sstate->vcache.float_base;
  return obj_instance_of(AS_OBJ(value), constraint) != NULL;
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
        return my_asprintf("Tried to set existing key '%.*s', but object %p was frozen.", (int) key->len, key->ptr, (void*) current);
      }
      if (!value_fits_constraint(state->shared, value, entry->constraint)) {
        return "type constraint violated on assignment";
      }
      entry->value = value;
      return NULL;
    }
    current = current->parent;
  }
  return my_asprintf("Key '%.*s' not found in object %p.", (int) key->len, key->ptr, (void*) obj);
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
    if (!res) return NULL;
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
    state->shared->gcstate.num_obj_allocated ++;
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
  obj->static_ptr = true;
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
  OBJECT_SET_STRING(state, (Object*) obj, "length", INT2VAL(length));
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
  if (update_len) OBJECT_SET_STRING(state, (Object*) aobj, "length", INT2VAL(aobj->length));
}

Value make_ptr(VMState *state, void *ptr) {
  PointerObject *obj = alloc_object_internal(state, sizeof(PointerObject), false);
  obj->base.parent = state->shared->vcache.pointer_base;
  // see alloc_int
  obj->base.flags = OBJ_NOINHERIT;
  obj->ptr = ptr;
  return OBJ2VAL((Object*) obj);
}

// used to allocate "special functions" like ffi functions, that need to store more data
Value make_fn_custom(VMState *state, VMFunctionPointer fn, int size_custom) {
  assert(size_custom >= sizeof(FunctionObject));
  FunctionObject *obj = alloc_object_internal(state, size_custom, false);
  obj->base.parent = state->shared->vcache.function_base;
  obj->base.flags |= OBJ_NOINHERIT;
  obj->fn_ptr = fn;
  return OBJ2VAL((Object*) obj);
}

Value make_fn(VMState *state, VMFunctionPointer fn) {
  return make_fn_custom(state, fn, sizeof(FunctionObject));
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
  int num_samples;
  bool direct;
} ProfilerRecord;

int prec_sort_outside_in_fn(const void *a, const void *b) {
  const ProfilerRecord *rec_a = a, *rec_b = b;
  // ranges that start earlier
  if (rec_a->range->text_from < rec_b->range->text_from) return -1;
  if (rec_a->range->text_from > rec_b->range->text_from) return 1;
  // then ranges that end later (outermost)
  if (rec_a->range->text_len > rec_b->range->text_len) return -1;
  if (rec_a->range->text_len < rec_b->range->text_len) return 1;
  // otherwise they're identical (direct/indirect collision)
  return 0;
}

struct _OpenRange;
typedef struct _OpenRange OpenRange;

struct _OpenRange {
  ProfilerRecord *record;
  OpenRange *prev;
};

static void push_record(OpenRange **range_p, ProfilerRecord *rec) {
  OpenRange *new_range = malloc(sizeof(OpenRange));
  new_range->record = rec;
  new_range->prev = *range_p;
  *range_p = new_range;
}

static void drop_record(OpenRange **range_p) {
  OpenRange *prev_range = (*range_p)->prev;
  free(*range_p);
  *range_p = prev_range;
}

// TODO move into separate file
void save_profile_output(char *filename, VMProfileState *profile_state) {
  FILE *file = fopen(filename, "w");
  if (file == NULL) {
    fprintf(stderr, "error creating profiler output file: %s\n", strerror(errno));
    abort();
  }
  
  HashTable *direct_table = &profile_state->direct_table;
  HashTable *indirect_table = &profile_state->indirect_table;
  int num_direct_records = direct_table->entries_stored;
  int num_indirect_records = indirect_table->entries_stored;
  int num_records = num_direct_records + num_indirect_records;
  
  ProfilerRecord *record_entries = calloc(sizeof(ProfilerRecord), num_records);
  
  // 1 so we can divide by it
  int max_samples_direct = 1, max_samples_indirect = 1;
  int sum_samples_direct = 1, sum_samples_indirect = 1;
  int k = 0;
  for (int i = 0; i < direct_table->entries_num; ++i) {
    TableEntry *entry = &direct_table->entries_ptr[i];
    if (entry->key_ptr) {
      FileRange *range = (FileRange*) entry->key_ptr; // This hurts my soul.
      int samples = entry->value.i;
      // printf("dir entry %i of %i: %i %.*s (%i)\n", i, direct_table->entries_num, (int) (range->text_to - range->text_from), (int) (range->text_to - range->text_from), range->text_from, samples);
      if (samples > max_samples_direct) max_samples_direct = samples;
      sum_samples_direct += samples;
      record_entries[k++] = (ProfilerRecord) { range, samples, true };
    }
  }
  for (int i = 0; i < indirect_table->entries_num; ++i) {
    TableEntry *entry = &indirect_table->entries_ptr[i];
    if (entry->key_ptr) {
      FileRange *range = (FileRange*) entry->key_ptr;
      int samples = entry->value.i;
      // printf("indir entry %i of %i: %i %.*s (%i)\n", i, indirect_table->entries_num, (int) (range->text_to - range->text_from), (int) (range->text_to - range->text_from), range->text_from, samples);
      if (samples > max_samples_indirect) max_samples_indirect = samples;
      sum_samples_indirect += samples;
      record_entries[k++] = (ProfilerRecord) { range, samples, false };
    }
  }
  assert(k == num_records);
  
  qsort(record_entries, num_records, sizeof(ProfilerRecord), prec_sort_outside_in_fn);
  
#define CUR_ENTRY (&record_entries[cur_entry_id])
  
  fprintf(file, "<!DOCTYPE html>\n");
  fprintf(file, "<html lang=\"\"><head><title>Profiler</title>\n");
  // fprintf(file, "<style>span { border-left: 1px solid #eee; border-right: 1px solid #ddd; border-bottom: 1px solid #fff; }</style>\n");
  fprintf(file, "<style>span { position: relative; }</style>\n");
  fprintf(file, "<script src=\"https://code.jquery.com/jquery-3.1.0.min.js\"></script>\n");
  fprintf(file, "<link rel=\"stylesheet\" href=\"https://maxcdn.bootstrapcdn.com/bootstrap/3.3.7/css/bootstrap.min.css\" crossorigin=\"anonymous\">\n");
  fprintf(file, "<link rel=\"stylesheet\" href=\"https://maxcdn.bootstrapcdn.com/bootstrap/3.3.7/css/bootstrap-theme.min.css\" crossorigin=\"anonymous\">\n");
  fprintf(file, "<script src=\"https://maxcdn.bootstrapcdn.com/bootstrap/3.3.7/js/bootstrap.min.js\" crossorigin=\"anonymous\"></script>\n");
  fprintf(file, "</head><body style=\"margin: 20px;\">\n");
  
  fprintf(file, "<div class=\"accordion\" id=\"file_accordion\">\n");
  
  int num_files = 0;
  FileEntry *files = get_files(&num_files);
  for (int i = 0; i < num_files; i++) {
    TextRange source = files[i].range;
    OpenRange *open_range_head = NULL;
    
    fprintf(file, "<div class=\"accordion-group\">\n");
    fprintf(file, "<div class=\"accordion-heading\"><a class=\"accordion-toggle\" data-toggle=\"collapse\" data-parent=\"#file_accordion\" href=\"#filecollapse%i\">\n", i);
    fprintf(file, "%s\n", files[i].file);
    fprintf(file, "</a></div>\n");
    fprintf(file, "<div id=\"filecollapse%i\" class=\"accordion-body collapse\"><div class=\"accordion-inner\">\n", i);
    fprintf(file, "<pre>");
    char *cur_char = source.start;
    int cur_entry_id = 0;
    int zindex = 100000; // if there's more spans than this we are anyways fucked
    while (cur_char != source.end) {
      // close all extant
      while (open_range_head && open_range_head->record->range->text_from + open_range_head->record->range->text_len == cur_char) {
        // fprintf(stderr, "%li: close tag\n", open_range_head->record->text_to - source.start);
        drop_record(&open_range_head);
        fprintf(file, "</span>");
      }
      // skip any preceding
      while (cur_entry_id < num_records && CUR_ENTRY->range->text_from < cur_char) {
        cur_entry_id ++;
      }
      // open any new
      while (cur_entry_id < num_records && CUR_ENTRY->range->text_from == cur_char) {
        // fprintf(stderr, "%li with %li: open tag\n", CUR_ENTRY.text_from - source.start, CUR_ENTRY.text_to - CUR_ENTRY.text_from);
        push_record(&open_range_head, CUR_ENTRY);
        int *samples_dir_p = NULL, *samples_indir_p = NULL;
        OpenRange *cur = open_range_head;
        // find the last (innermost) set dir/indir values
        while (cur && (!samples_dir_p || !samples_indir_p)) {
          if (!samples_dir_p && cur->record->direct) samples_dir_p = &cur->record->num_samples;
          if (!samples_indir_p && !cur->record->direct) samples_indir_p = &cur->record->num_samples;
          cur = cur->prev;
        }
        int samples_dir = samples_dir_p ? *samples_dir_p : 0;
        int samples_indir = samples_indir_p ? *samples_indir_p : 0;
        double percent_dir = (samples_dir * 100.0) / sum_samples_direct;
        int hex_dir = 255 - (samples_dir * 255LL) / max_samples_direct;
        // sum direct == max indirect
        double percent_indir = (samples_indir * 100.0) / sum_samples_direct;
        // int hex_indir = 255 - (samples_indir * 255LL) / sum_samples_direct;
        int weight_indir = 100 + 100 * ((samples_indir * 8LL) / sum_samples_direct);
        float border_indir = samples_indir * 3.0 / sum_samples_direct;
        int fontsize_indir = 100 + (samples_indir * 10LL) / sum_samples_direct;
        int bordercol_indir = 15 - (int)(15*((border_indir < 1)?border_indir:1));
        /*printf("%li with %li: open tag %i and %i over (%i, %i) and (%i, %i): %02x and %i / %f\n",
              CUR_ENTRY.text_from - source.start, CUR_ENTRY.text_to - CUR_ENTRY.text_from,
              samples_dir, samples_indir,
              max_samples_direct, sum_samples_direct,
              max_samples_indirect, sum_samples_indirect,
              hex_dir, weight_indir, border_indir);*/
        fprintf(file, "<span title=\"%.2f%% active, %.2f%% in backtrace\""
          " style=\"",
          percent_dir, percent_indir);
        if (hex_dir <= 250/* || hex_dir <= 250*/) {
          fprintf(file, "background-color:#ff%02x%02x;",
            hex_dir, hex_dir);
        }
        fprintf(file, "font-weight:%i;border-bottom:%fpx solid #%1x%1x%1x;font-size: %i%%;",
                weight_indir, border_indir, bordercol_indir, bordercol_indir, bordercol_indir, fontsize_indir);
        fprintf(file, "z-index: %i;", --zindex);
        fprintf(file, "\">");
        cur_entry_id ++;
      }
      // close all 0-size new
      while (open_range_head && open_range_head->record->range->text_from + open_range_head->record->range->text_len == cur_char) {
        // fprintf(stderr, "%li: close tag\n", open_range_head->record->text_to - source.start);
        drop_record(&open_range_head);
        fprintf(file, "</span>");
      }
      if (*cur_char == '<') fprintf(file, "&lt;");
      else if (*cur_char == '>') fprintf(file, "&gt;");
      // else if (*cur_char == '\n') fprintf(file, "</pre>\n<pre>");
      else fprintf(file, "%c", *cur_char);
      cur_char ++;
    }
    fprintf(file, "</pre>\n");
    fprintf(file, "</div></div></div>\n");
  }
#undef CUR_ENTRY
  
  fprintf(file, "</div>\n");
  fprintf(file, "</body></html>");
  fclose(file);
}
