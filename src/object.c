#include <stdio.h>

#include <fcntl.h>
#include <errno.h>
#include <unistd.h>

#include "object.h"

#define DEBUG_MEM 0

void gc_run(VMState *state); // defined here so we can call it in alloc

void *int_freelist = NULL;
void *obj_freelist = NULL;
void *table4_freelist = NULL, *table8_freelist = NULL, *table16_freelist = NULL;

void *cache_alloc(int size) {
  // return calloc(size, 1);
  void *res = NULL;
  switch (size) {
    case sizeof(IntObject):
      if (int_freelist) {
        res = int_freelist;
        int_freelist = *(void**) int_freelist;
      }
      break;
    case sizeof(TableEntry) * 4:
      if (table4_freelist) {
        res = table4_freelist;
        table4_freelist = *(void**) table4_freelist;
      }
      break;
    case sizeof(TableEntry) * 8:
      if (table8_freelist) {
        res = table8_freelist;
        table8_freelist = *(void**) table8_freelist;
      }
      break;
    case sizeof(TableEntry) * 16:
      if (table16_freelist) {
        res = table16_freelist;
        table16_freelist = *(void**) table16_freelist;
      }
      break;
    case sizeof(Object):
      if (obj_freelist) {
        res = obj_freelist;
        obj_freelist = *(void**) obj_freelist;
      }
      break;
    default: break;
  }
  if (!res) return calloc(size, 1);
  memset(res, 0, size);
  return res;
}

void cache_free(int size, void *ptr) {
  // free(ptr); return;
  switch (size) {
    case sizeof(IntObject):
      *(void**) ptr = int_freelist;
      int_freelist = ptr;
      break;
    case sizeof(TableEntry) * 4:
      *(void**) ptr = table4_freelist;
      table4_freelist = ptr;
      break;
    case sizeof(TableEntry) * 8:
      *(void**) ptr = table8_freelist;
      table8_freelist = ptr;
      break;
    case sizeof(TableEntry) * 16:
      *(void**) ptr = table16_freelist;
      table16_freelist = ptr;
      break;
    case sizeof(Object):
      *(void**) ptr = obj_freelist;
      obj_freelist = ptr;
      break;
    default:
      free(ptr);
      break;
  }
}

Object *object_lookup(Object *obj, const char *key, bool *key_found_p) {
  while (obj) {
    bool key_found;
    Object *value = table_lookup(&obj->tbl, key, strlen(key), &key_found);
    if (key_found) { if (key_found_p) *key_found_p = true; return value; }
    obj = obj->parent;
  }
  if (key_found_p) *key_found_p = false;
  return NULL;
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
    Object **ptr = (Object**) table_lookup_ref(&current->tbl, key, strlen(key));
    if (ptr != NULL) {
      if (current->flags & OBJ_IMMUTABLE) {
        char *error = NULL;
        if (-1 == asprintf(&error, "Tried to set existing key '%s', but object %p was immutable.", key, (void*) current)) abort();
        return error;
      }
      assert(!(current->flags & OBJ_IMMUTABLE));
      *ptr = value;
      return NULL;
    }
    current = current->parent;
  }
  char *error = NULL;
  if (-1 == asprintf(&error, "Key '%s' not found in object %p.", key, (void*) obj)) abort();
  return error;
}

// change a property but only if it exists somewhere in the prototype chain
bool object_set_shadowing(Object *obj, const char *key, Object *value) {
  assert(obj != NULL);
  Object *current = obj;
  while (current) {
    Object **ptr = (Object**) table_lookup_ref(&current->tbl, key, strlen(key));
    if (ptr) {
      // so create it in obj (not current!)
      object_set(obj, key, value);
      return true;
    }
    current = current->parent;
  }
  return false;
}

void object_set(Object *obj, const char *key, Object *value) {
  assert(obj != NULL);
  void **freeptr;
  Object **ptr = (Object **) table_lookup_ref_alloc(&obj->tbl, key, strlen(key), &freeptr);
  if (ptr) {
    assert(!(obj->flags & OBJ_IMMUTABLE));
  } else {
    assert(!(obj->flags & OBJ_CLOSED));
    ptr = (Object **) freeptr;
  }
  *ptr = value;
}

static void *alloc_object_internal(VMState *state, int size) {
  if (state->num_obj_allocated > state->next_gc_run) {
    gc_run(state);
    // run gc after 50% growth or 10000 allocated or thereabouts
    state->next_gc_run = (int) (state->num_obj_allocated * 1.5) + 10000;
  }
  
  Object *res = cache_alloc(size);
  res->prev = state->last_obj_allocated;
  res->size = size;
  state->last_obj_allocated = res;
  state->num_obj_allocated ++;
  
#if DEBUG_MEM
  fprintf(stderr, "alloc object %p\n", (void*) obj);
#endif
  
  return res;
}

Object *alloc_object(VMState *state, Object *parent) {
  Object *obj = alloc_object_internal(state, sizeof(Object));
  obj->parent = parent;
  return obj;
}

Object *alloc_int(VMState *state, int value) {
  Object *int_base = object_lookup(state->root, "int", NULL);
  assert(int_base);
  IntObject *obj = alloc_object_internal(state, sizeof(IntObject));
  obj->base.parent = int_base;
  // why though?
  // obj->base.flags |= OBJ_IMMUTABLE | OBJ_CLOSED;
  obj->value = value;
  return (Object*) obj;
}

Object *alloc_bool(VMState *state, int value) {
  Object *bool_base = object_lookup(state->root, "bool", NULL);
  assert(bool_base);
  BoolObject *obj = alloc_object_internal(state, sizeof(BoolObject));
  obj->base.parent = bool_base;
  // obj->base.flags |= OBJ_IMMUTABLE | OBJ_CLOSED;
  obj->value = value;
  return (Object*) obj;
}

Object *alloc_float(VMState *state, float value) {
  Object *float_base = object_lookup(state->root, "float", NULL);
  assert(float_base);
  FloatObject *obj = alloc_object_internal(state, sizeof(FloatObject));
  obj->base.parent = float_base;
  // obj->base.flags |= OBJ_IMMUTABLE | OBJ_CLOSED;
  obj->value = value;
  return (Object*) obj;
}

Object *alloc_string(VMState *state, const char *value) {
  Object *string_base = object_lookup(state->root, "string", NULL);
  assert(string_base);
  int len = strlen(value);
  // allocate the string as part of the object, so that it gets freed with the object
  StringObject *obj = alloc_object_internal(state, sizeof(StringObject) + len + 1);
  obj->base.parent = string_base;
  // obj->base.flags |= OBJ_IMMUTABLE | OBJ_CLOSED;
  obj->value = ((char*) obj) + sizeof(StringObject);
  strncpy(obj->value, value, len + 1);
  return (Object*) obj;
}

Object *alloc_string_foreign(VMState *state, char *value) {
  Object *string_base = object_lookup(state->root, "string", NULL);
  assert(string_base);
  // allocate the string as part of the object, so that it gets freed with the object
  StringObject *obj = alloc_object_internal(state, sizeof(StringObject));
  obj->base.parent = string_base;
  // obj->base.flags |= OBJ_IMMUTABLE | OBJ_CLOSED;
  obj->value = value;
  return (Object*) obj;
}

Object *alloc_array(VMState *state, Object **ptr, int length) {
  Object *array_base = object_lookup(state->root, "array", NULL);
  assert(array_base);
  ArrayObject *obj = alloc_object_internal(state, sizeof(ArrayObject));
  obj->base.parent = array_base;
  obj->ptr = ptr;
  obj->length = length;
  gc_disable(state);
  object_set((Object*) obj, "length", alloc_int(state, length));
  gc_enable(state);
  return (Object*) obj;
}

Object *alloc_ptr(VMState *state, void *ptr) { // TODO unify with alloc_fn
  Object *ptr_base = object_lookup(state->root, "pointer", NULL);
  assert(ptr_base);
  PointerObject *obj = alloc_object_internal(state, sizeof(PointerObject));
  obj->base.parent = ptr_base;
  obj->ptr = ptr;
  return (Object*) obj;
}

Object *alloc_fn(VMState *state, VMFunctionPointer fn) {
  Object *fn_base = object_lookup(state->root, "function", NULL);
  assert(fn_base);
  FunctionObject *obj = alloc_object_internal(state, sizeof(FunctionObject));
  obj->base.parent = fn_base;
  obj->fn_ptr = fn;
  return (Object*) obj;
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
      int samples = *(int*) &entry->value;
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
      int samples = *(int*) &entry->value;
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
  dprintf(fd, "<style>span { border: 1px solid gray60; }</style>\n");
  dprintf(fd, "</head><body>\n");
  dprintf(fd, "<pre>\n");
  
  char *cur_char = source.start;
  while (cur_char != source.end) {
    // close all extant
    while (open_range_head && open_range_head->record->text_to == cur_char) {
      // fprintf(stderr, "%li: close tag\n", open_range_head->record->text_to - source.start);
      drop_record(&open_range_head);
      dprintf(fd, "</span>");
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
      double percent_indir = (samples_indir * 100.0) / sum_samples_indirect;
      int hex_dir = 255 - (samples_dir * 255LL) / max_samples_direct;
      // int hex_indir = 255 - (samples_indir * 255LL) / max_samples_indirect;
      int weight_indir = 100 + (samples_indir * 800LL) / max_samples_indirect;
      float border_indir = samples_indir * 3.0 / max_samples_indirect;
      int fontsize_indir = 100 + (samples_indir * 10LL) / max_samples_indirect;
      /*printf("%li with %li: open tag %i and %i over %i and %i: %02x and %i / %f\n",
             CUR_ENTRY.text_from - source.start, CUR_ENTRY.text_to - CUR_ENTRY.text_from,
             samples_dir, samples_indir,
             max_samples_direct, max_samples_indirect,
             hex_dir, weight_indir, border_indir);*/
      dprintf(fd, "<span title=\"%.2f%% active, %.2f%% in backtrace\""
        "style=\"background-color:#ff%02x%02x;font-weight:%i;border-bottom:%fpx solid black;font-size: %i%%;\">",
        percent_dir, percent_indir,
        hex_dir, hex_dir, weight_indir, border_indir, fontsize_indir);
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

