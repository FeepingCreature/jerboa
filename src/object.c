#include <stdio.h>

#include <fcntl.h>
#include <errno.h>
#include <unistd.h>

#include "object.h"

#define DEBUG_MEM 0

#define FREELIST_LIMIT 32

void *freelist[FREELIST_LIMIT] = {0};

void *cache_alloc(int size) {
  if (UNLIKELY(size == 0)) return NULL;
  size = (size + 15) & ~15; // align to 16
  int slot = size >> 4;
  void *res;
  if (UNLIKELY(size == 64 && !freelist[slot])) {
    // feed the freelist with a single big alloc
    void *bigalloc = malloc(64 * 1024);
    // stitch backwards
    void *cursor = (char*) bigalloc + 64 * (1024 - 1);
    void *needle = freelist[slot];
    for (int i = 0; i < 1024; ++i) {
      *(void**) cursor = needle;
      needle = cursor;
      cursor = (char*) cursor - 64;
    }
    freelist[slot] = needle;
  }
  if (slot < FREELIST_LIMIT && freelist[slot]) {
    res = freelist[slot];
    freelist[slot] = *(void**) freelist[slot];
  } else {
    // printf(": alloc %i\n", size);
    res = malloc(size);
  }
  bzero(res, size);
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

Object **object_lookup_ref_with_hash(Object *obj, const char *key_ptr, size_t key_len, size_t hashv) {
  while (obj) {
    Object **value_p = (Object**) &table_lookup_with_hash(&obj->tbl, key_ptr, strlen(key_ptr), hashv)->value;
    if (value_p) return value_p;
    obj = obj->parent;
  }
  return NULL;
}

Object **object_lookup_ref(Object *obj, const char *key_ptr) {
  size_t len = strlen(key_ptr);
  size_t hashv = hash(key_ptr, len);
  return object_lookup_ref_with_hash(obj, key_ptr, len, hashv);
}

Object *object_lookup_with_hash(Object *obj, const char *key_ptr, size_t key_len, size_t hashv, bool *key_found_p) {
  if (!key_found_p) {
    while (obj) {
      TableEntry *entry = table_lookup_with_hash(&obj->tbl, key_ptr, strlen(key_ptr), hashv);
      if (entry) return entry->value;
      obj = obj->parent;
    }
    return NULL;
  }
  while (obj) {
    TableEntry *entry = table_lookup_with_hash(&obj->tbl, key_ptr, strlen(key_ptr), hashv);
    if (entry) { *key_found_p = true; return entry->value; }
    obj = obj->parent;
  }
  *key_found_p = false;
  return NULL;
}

Object *object_lookup(Object *obj, const char *key_ptr, bool *key_found_p) {
  size_t len = strlen(key_ptr);
  size_t hashv = hash(key_ptr, len);
  return object_lookup_with_hash(obj, key_ptr, len, hashv, key_found_p);
}

void obj_mark(VMState *state, Object *obj) {
  if (!obj) return;
  
  if (obj->flags & OBJ_GC_MARK) return; // break cycles
  
  obj->flags |= OBJ_GC_MARK;
  
  obj_mark(state, obj->parent);
  
  HashTable *tbl = &obj->tbl;
  for (int i = 0; i < tbl->entries_num; ++i) {
    TableEntry *entry = &tbl->entries_ptr[i];
    if (entry->name_ptr) {
      obj_mark(state, entry->value);
    }
  }
  
  Object *current = obj;
  while (current) {
    if (current->mark_fn) {
      current->mark_fn(state, obj);
    }
    current = current->parent;
  }
}

void obj_free(Object *obj) {
  cache_free(sizeof(TableEntry) * obj->tbl.entries_num, obj->tbl.entries_ptr);
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

Object *obj_instance_of_or_equal(Object *obj, Object *proto) {
  if (obj == proto) return obj;
  return obj_instance_of(obj, proto);
}

// change a property in-place
// returns an error string or NULL
char *object_set_existing(Object *obj, const char *key, Object *value) {
  assert(obj != NULL);
  Object *current = obj;
  while (current) {
    TableEntry *entry = table_lookup(&current->tbl, key, strlen(key));
    if (entry != NULL) {
      if (current->flags & OBJ_FROZEN) {
        char *error = NULL;
        if (-1 == asprintf(&error, "Tried to set existing key '%s', but object %p was frozen.", key, (void*) current)) abort();
        return error;
      }
      if (entry->value_aux && (!value || value->parent != (Object*) entry->value_aux)) {
        return "type constraint violated on assignment";
      }
      entry->value = value;
      return NULL;
    }
    current = current->parent;
  }
  char *error = NULL;
  if (-1 == asprintf(&error, "Key '%s' not found in object %p.", key, (void*) obj)) abort();
  return error;
}

// change a property but only if it exists somewhere in the prototype chain
// returns error or null on success
char *object_set_shadowing(Object *obj, const char *key, Object *value, bool *value_set) {
  assert(obj != NULL);
  Object *current = obj;
  while (current) {
    TableEntry *entry = table_lookup(&current->tbl, key, strlen(key));
    if (entry) {
      if (entry->value_aux && (!value || value->parent != (Object*) entry->value_aux)) {
        return "type constraint violated on shadowing assignment";
      }
      // so create it in obj (not current!)
      object_set(obj, key, value);
      *value_set = true;
      return NULL;
    }
    current = current->parent;
  }
  *value_set = false;
  return NULL;
}

// returns error or null
char *object_set_constraint(VMState *state, Object *obj, const char *key_ptr, size_t key_len, Object *constraint) {
  assert(obj != NULL);
  TableEntry *entry = table_lookup(&obj->tbl, key_ptr, key_len);
  if (!entry) return "tried to set constraint on a key that was not yet defined!";
  if (entry->value_aux) return "tried to set constraint, but key already had a constraint!";
  Object *existing_value = entry->value;
  if (!existing_value || existing_value->parent != constraint) {
    return my_asprintf("value failed type constraint: constraint was %s, but value was %s",
                       get_type_info(state, constraint), get_type_info(state, existing_value));
  }
  // There is no need to check flags here - constraints cannot be changed and don't modify existing data.
  entry->value_aux = (void*) constraint;
  return NULL;
}

// returns error or null
char *object_set(Object *obj, const char *key, Object *value) {
  assert(obj != NULL);
  TableEntry *freeptr;
  // TODO check flags beforehand to avoid clobbering tables that are frozen
  TableEntry *entry = table_lookup_alloc(&obj->tbl, key, strlen(key), &freeptr);
  if (entry) {
    assert(!(obj->flags & OBJ_FROZEN));
    if (entry->value_aux && (!value || value->parent != (Object*) entry->value_aux)) {
      return "type constraint violated on assignment";
    }
    entry->value = (void*) value;
  } else {
    assert(!(obj->flags & OBJ_CLOSED));
    freeptr->value = (void*) value;
  }
  return NULL;
}

void vm_record_profile(VMState *state);
void *alloc_object_internal(VMState *state, int size) {
  Object *res = cache_alloc(size);
  res->prev = state->shared->gcstate.last_obj_allocated;
  res->size = size;
  state->shared->gcstate.last_obj_allocated = res;
  state->shared->gcstate.num_obj_allocated ++;
  
#if DEBUG_MEM
  fprintf(stderr, "alloc object %p\n", (void*) obj);
#endif
  
  // if(state->runstate == VM_RUNNING) vm_record_profile(state);
  
  return res;
}

Object *alloc_object(VMState *state, Object *parent) {
  Object *obj = alloc_object_internal(state, sizeof(Object));
  obj->parent = parent;
  return obj;
}

Object *alloc_int(VMState *state, int value) {
  IntObject *obj = alloc_object_internal(state, sizeof(IntObject));
  obj->base.parent = state->shared->vcache.int_base;
  // let us replace obj_instance_of with a simple "parent =="
  // also prevent variations and modifications
  // this is important to allow the optimizer to treat all ints interchangeably
  // which permits type specialization
  obj->base.flags = OBJ_NOINHERIT | OBJ_FROZEN | OBJ_CLOSED;
  
  obj->value = value;
  return (Object*) obj;
}

Object *alloc_bool_uncached(VMState *state, bool value) {
  BoolObject *obj = alloc_object_internal(state, sizeof(BoolObject));
  obj->base.parent = state->shared->vcache.bool_base;
  // see alloc_int
  obj->base.flags = OBJ_NOINHERIT | OBJ_FROZEN | OBJ_CLOSED;
  obj->value = value;
  return (Object*) obj;
}

Object *alloc_bool(VMState *state, bool value) {
  if (value == true) {
    return state->shared->vcache.bool_true;
  } else {
    return state->shared->vcache.bool_false;
  }
}

Object *alloc_float(VMState *state, float value) {
  FloatObject *obj = alloc_object_internal(state, sizeof(FloatObject));
  obj->base.parent = state->shared->vcache.float_base;
  // see alloc_int
  obj->base.flags = OBJ_NOINHERIT | OBJ_FROZEN | OBJ_CLOSED;
  obj->value = value;
  return (Object*) obj;
}

Object *alloc_string(VMState *state, const char *ptr, int len) {
  // allocate the string as part of the object, so that it gets freed with the object
  StringObject *obj = alloc_object_internal(state, sizeof(StringObject) + len + 1);
  obj->base.parent = state->shared->vcache.string_base;
  // obj->base.flags = OBJ_IMMUTABLE | OBJ_CLOSED;
  obj->value = ((char*) obj) + sizeof(StringObject);
  strncpy(obj->value, ptr, len);
  obj->value[len] = 0;
  return (Object*) obj;
}

Object *alloc_string_foreign(VMState *state, char *value) {
  // allocate the string as part of the object, so that it gets freed with the object
  StringObject *obj = alloc_object_internal(state, sizeof(StringObject));
  obj->base.parent = state->shared->vcache.string_base;
  // obj->base.flags = OBJ_IMMUTABLE | OBJ_CLOSED;
  obj->value = value;
  return (Object*) obj;
}

Object *alloc_array(VMState *state, Object **ptr, IntObject *length) {
  ArrayObject *obj = alloc_object_internal(state, sizeof(ArrayObject));
  obj->base.parent = state->shared->vcache.array_base;
  obj->ptr = ptr;
  obj->length = length->value;
  object_set((Object*) obj, "length", (Object*) length);
  return (Object*) obj;
}

Object *alloc_ptr(VMState *state, void *ptr) {
  PointerObject *obj = alloc_object_internal(state, sizeof(PointerObject));
  obj->base.parent = state->shared->vcache.pointer_base;
  // see alloc_int
  obj->base.flags = OBJ_NOINHERIT;
  obj->ptr = ptr;
  return (Object*) obj;
}

// used to allocate "special functions" like ffi functions, that need to store more data
Object *alloc_fn_custom(VMState *state, VMFunctionPointer fn, int size_custom) {
  assert(size_custom >= sizeof(FunctionObject));
  FunctionObject *obj = alloc_object_internal(state, size_custom);
  obj->base.parent = state->shared->vcache.function_base;
  obj->fn_ptr = fn;
  return (Object*) obj;
}

Object *alloc_fn(VMState *state, VMFunctionPointer fn) {
  return alloc_fn_custom(state, fn, sizeof(FunctionObject));
}

// TODO move elsewhere
typedef struct {
  const char *text_from, *text_to;
  int num_samples;
  bool direct;
} ProfilerRecord;

int prec_sort_outside_in_fn(const void *a, const void *b) {
  const ProfilerRecord *rec_a = a, *rec_b = b;
  // ranges that start earlier
  if (rec_a->text_from < rec_b->text_from) return -1;
  if (rec_a->text_from > rec_b->text_from) return 1;
  // then ranges that end later (outermost)
  if (rec_a->text_to > rec_b->text_to) return -1;
  if (rec_a->text_to < rec_b->text_to) return 1;
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
  *range_p = (*range_p)->prev;
}

// TODO move into separate file
void save_profile_output(char *file, TextRange source, VMProfileState *profile_state) {
  int fd = creat(file, 0644);
  if (fd == -1) {
    fprintf(stderr, "error creating profiler output file: %s\n", strerror(errno));
    abort();
  }
  
  HashTable *direct_table = &profile_state->direct_table;
  HashTable *indirect_table = &profile_state->indirect_table;
  int num_direct_records = direct_table->entries_stored;
  int num_indirect_records = indirect_table->entries_stored;
  int num_records = num_direct_records + num_indirect_records;
  
  ProfilerRecord *record_entries = calloc(sizeof(ProfilerRecord), num_records);
  
  int max_samples_direct = 0, max_samples_indirect = 0;
  int sum_samples_direct = 0, sum_samples_indirect = 0;
  int k = 0;
  for (int i = 0; i < direct_table->entries_num; ++i) {
    TableEntry *entry = &direct_table->entries_ptr[i];
    if (entry->name_ptr) {
      FileRange *range = *(FileRange**) entry->name_ptr; // This hurts my soul.
      int samples = (intptr_t) entry->value;
      // printf("dir entry %i of %i: %i %.*s (%i)\n", i, direct_table->entries_num, (int) (range->text_to - range->text_from), (int) (range->text_to - range->text_from), range->text_from, samples);
      if (samples > max_samples_direct) max_samples_direct = samples;
      sum_samples_direct += samples;
      record_entries[k++] = (ProfilerRecord) { range->text_from, range->text_to, samples, true };
    }
  }
  for (int i = 0; i < indirect_table->entries_num; ++i) {
    TableEntry *entry = &indirect_table->entries_ptr[i];
    if (entry->name_ptr) {
      FileRange *range = *(FileRange**) entry->name_ptr;
      int samples = (intptr_t) entry->value;
      // printf("indir entry %i of %i: %i %.*s (%i)\n", i, indirect_table->entries_num, (int) (range->text_to - range->text_from), (int) (range->text_to - range->text_from), range->text_from, samples);
      if (samples > max_samples_indirect) max_samples_indirect = samples;
      sum_samples_indirect += samples;
      record_entries[k++] = (ProfilerRecord) { range->text_from, range->text_to, samples, false };
    }
  }
  assert(k == num_records);
  
  qsort(record_entries, num_records, sizeof(ProfilerRecord), prec_sort_outside_in_fn);
  
  int cur_entry_id = 0;
#define CUR_ENTRY record_entries[cur_entry_id]
  
  OpenRange *open_range_head = NULL;
  
  dprintf(fd, "<!DOCTYPE html>\n");
  dprintf(fd, "<html><head>\n");
  // dprintf(fd, "<style>span { border-left: 1px solid #eee; border-right: 1px solid #ddd; border-bottom: 1px solid #fff; }</style>\n");
  dprintf(fd, "<style>span { position: relative; }</style>\n");
  dprintf(fd, "</head><body>\n");
  dprintf(fd, "<pre>\n");
  
  char *cur_char = source.start;
  int zindex = 100000; // if there's more spans than this we are anyways fucked
  while (cur_char != source.end) {
    // close all extant
    while (open_range_head && open_range_head->record->text_to == cur_char) {
      // fprintf(stderr, "%li: close tag\n", open_range_head->record->text_to - source.start);
      drop_record(&open_range_head);
      dprintf(fd, "</span>");
    }
    // skip any preceding
    while (cur_entry_id < num_records && CUR_ENTRY.text_from < cur_char) {
      cur_entry_id ++;
    }
    // open any new
    while (cur_entry_id < num_records && CUR_ENTRY.text_from == cur_char) {
      // fprintf(stderr, "%li with %li: open tag\n", CUR_ENTRY.text_from - source.start, CUR_ENTRY.text_to - CUR_ENTRY.text_from);
      push_record(&open_range_head, &CUR_ENTRY);
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
      dprintf(fd, "<span title=\"%.2f%% active, %.2f%% in backtrace\""
        " style=\"",
        percent_dir, percent_indir);
      if (hex_dir <= 250/* || hex_dir <= 250*/) {
        dprintf(fd, "background-color:#ff%02x%02x;",
          hex_dir, hex_dir);
      }
      dprintf(fd, "font-weight:%i;border-bottom:%fpx solid #%1x%1x%1x;font-size: %i%%;",
              weight_indir, border_indir, bordercol_indir, bordercol_indir, bordercol_indir, fontsize_indir);
      dprintf(fd, "z-index: %i;", --zindex);
      dprintf(fd, "\">");
      cur_entry_id ++;
    }
    // close all 0-size new
    while (open_range_head && open_range_head->record->text_to == cur_char) {
      // fprintf(stderr, "%li: close tag\n", open_range_head->record->text_to - source.start);
      drop_record(&open_range_head);
      dprintf(fd, "</span>");
    }
    if (*cur_char == '<') dprintf(fd, "&lt;");
    else if (*cur_char == '>') dprintf(fd, "&gt;");
    // else if (*cur_char == '\n') dprintf(fd, "</pre>\n<pre>");
    else dprintf(fd, "%c", *cur_char);
    cur_char ++;
  }
  
#undef CUR_ENTRY
  
  dprintf(fd, "</pre>");
  dprintf(fd, "</body></html>");
  close(fd);
}
