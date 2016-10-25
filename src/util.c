#include "util.h"

#ifdef _WIN32
#include <windows.h>
#endif

#include "win32_compat.h"

#include <fcntl.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <unistd.h>
#include <stdarg.h>
#include <stdbool.h>

TextRange readfile(char *filename) {
  int file = open(filename, O_RDONLY);
  if (file == -1) { fprintf(stderr, "cannot open file '%s': %s\n", filename, strerror(errno)); abort(); }
  char *res_ptr = NULL; int res_len = 0;
  int bytes_read = 0;
  do {
    res_len = bytes_read + 1024;
    res_ptr = realloc(res_ptr, res_len);
    ssize_t numitems = read(file, res_ptr + bytes_read, res_len - bytes_read - 1);
    if (numitems == -1) { fprintf(stderr, "cannot read from file: %s\n", strerror(errno)); abort(); }
    if (numitems == 0) {
      res_ptr[bytes_read] = 0;
      return (TextRange){res_ptr, res_ptr + bytes_read};
    }
    bytes_read += numitems;
  } while (true);
}

bool file_exists(char *path) {
  return access(path, R_OK) != -1;
}

struct _FileRecord;
typedef struct _FileRecord FileRecord;

struct _FileRecord {
  FileRecord *prev;
  TextRange text;
  const char *name;
  int row_start, col_start;
};

struct _FunctionRecord;
typedef struct _FunctionRecord FunctionRecord;

struct _FunctionRecord {
  FunctionRecord *prev;
  TextRange range;
  const char *name;
};

// TODO mutex the shit out of this
FileRecord *record = NULL;
FunctionRecord *fn_record = NULL;

void register_file(TextRange text, const char *name, int row_start, int col_start) {
  FileRecord *newrecord = malloc(sizeof(FileRecord));
  newrecord->prev = record;
  newrecord->text = text;
  newrecord->name = name;
  newrecord->row_start = row_start;
  newrecord->col_start = col_start;
  record = newrecord;
}

void register_function(TextRange range, const char *name) {
  FunctionRecord *newrecord = malloc(sizeof(FunctionRecord));
  newrecord->prev = fn_record;
  newrecord->range = range;
  newrecord->name = name;
  fn_record = newrecord;
}

FileEntry *get_files(int *length_p) {
  int num_records = 0;
  FileRecord *cur = record;
  while (cur) {
    num_records ++;
    cur = cur->prev;
  }
  FileEntry *res = calloc(sizeof(FileEntry), num_records);
  int i = 0;
  cur = record;
  while (cur) {
    res[i++] = (FileEntry) { .file = cur->name, .range = cur->text };
    cur = cur->prev;
  }
  *length_p = num_records;
  return res;
}

// most find_text_pos calls are in the same line as last time
static __thread TextRange last_line = {0};
static __thread FileRecord *last_record = NULL;
static __thread FunctionRecord *last_fn_record = NULL;
static __thread int last_row_nr;

size_t utf8_strnlen(const char *ptr, size_t length) {
  size_t utf8_len = 0;
  const char *end = ptr + length;
  for (; ptr != end; ++ptr) {
    // see https://en.wikipedia.org/wiki/UTF-8#Description
    if ((*ptr & 0xC0) != 0x80) utf8_len++;
  }
  return utf8_len;
}

size_t utf8_strlen(const char *ptr) {
  size_t utf8_len = 0;
  for (; *ptr; ++ptr) {
    if ((*ptr & 0xC0) != 0x80) utf8_len++;
  }
  return utf8_len;
}

void utf8_step(const char **ptr, int num, const char **error_p) {
  const char *cur = *ptr;
  if (num) while (true) {
    if ((*cur & 0xC0) != 0x80) {
      // meaning cur is pointing at the start of a new codepoint
      if (num-- == 0) break;
    }
    if (!*cur) { *error_p = "ran out of utf8 string"; return; }
    cur++;
  }
  *ptr = cur;
}

static bool find_text_pos_from_to(char *text, FileRecord *record, char *text_to, const char **name_p, TextRange *line_p, int *row_p, int *col_p) {
  int row_nr = *row_p;
  TextRange line = *line_p;
  while (line.start <= text_to) {
    while (line.end < text_to && *line.end != '\n') line.end ++; // scan to newline
    line.end ++; // scan past newline (even if there isn't a newline - since a pointer to text_to should also be captured)
    if (text >= line.start && text < line.end) {
      last_line = line;
      last_record = record;
      last_row_nr = row_nr;
      
      int col_nr = text - line.start;
      *name_p = record->name;
      *line_p = line;
      *row_p = row_nr + record->row_start;
      *col_p = col_nr + ((row_nr == 0) ? record->col_start : 0);
      return true;
    }
    line.start = line.end;
    row_nr ++;
  }
  return false;
}

bool find_text_pos(char *text, const char **name_p, const char **function_p, TextRange *line_p, int *row_p, int *col_p) {
  if (!last_fn_record || text < last_fn_record->range.start || text >= last_fn_record->range.end) {
    FunctionRecord *cur = fn_record;
    while (cur) {
      if (text >= cur->range.start && text < cur->range.end) {
        last_fn_record = cur;
        break;
      }
      cur = cur->prev;
    }
    if (!cur) last_fn_record = NULL; // no hit
  }
  
  if (last_fn_record) *function_p = last_fn_record->name;
  else *function_p = NULL;
  
  // cache lookup
  if (text >= last_line.start && text < last_line.end) {
    int col_nr = text - last_line.start;
    *name_p = last_record->name;
    *line_p = last_line;
    *row_p = last_row_nr + last_record->row_start;
    *col_p = col_nr + ((last_row_nr == 0) ? last_record->col_start : 0);
    return true;
  }
  
  // read forward from last cache hit
  if (last_record && text >= last_record->text.start && text < last_record->text.end) {
    if (text >= last_line.end) {
      *row_p = last_row_nr + 1;
      *line_p = (TextRange) { last_line.end, last_line.end };
      bool res = find_text_pos_from_to(text, last_record, last_record->text.end, name_p, line_p, row_p, col_p);
      if (res) return true;
    }
  }
  
  // full rescan
  FileRecord *rec = record;
  while (rec) {
    if (text >= rec->text.start && text <= rec->text.end) {
      *row_p = 0;
      *line_p = (TextRange) { rec->text.start, rec->text.start };
      bool res = find_text_pos_from_to(text, rec, rec->text.end, name_p, line_p, row_p, col_p);
      assert(res); // logic error, wtf - text in range but not in any line??
      if (res) return true;
    }
    rec = rec->prev;
  }
  return false;
}

long long get_clock_and_difference(struct timespec *target_clock, struct timespec *compare_clock) {
  struct timespec holder;
  if (!target_clock) target_clock = &holder;
  
  int res = clock_gettime(CLOCK_MONOTONIC, target_clock);
  if (res != 0) abort();
  long ns_diff = target_clock->tv_nsec - compare_clock->tv_nsec;
  int s_diff = target_clock->tv_sec - compare_clock->tv_sec;
  return (long long) s_diff * 1000000000LL + (long long) ns_diff;
}

void format_bold(FILE *target) {
#ifdef _WIN32
  assert(target == stderr);
  HANDLE hdl = GetStdHandle(STD_ERROR_HANDLE);
  SetConsoleTextAttribute(hdl, FOREGROUND_RED|FOREGROUND_GREEN|FOREGROUND_BLUE|FOREGROUND_INTENSITY);
#else
  fprintf(target, "\x1b[1m");
#endif
}

void format_reset(FILE *target) {
#ifdef _WIN32
  assert(target == stderr);
  HANDLE hdl = GetStdHandle(STD_ERROR_HANDLE);
  SetConsoleTextAttribute(hdl, FOREGROUND_RED|FOREGROUND_GREEN|FOREGROUND_BLUE);
#else
  fprintf(target, "\x1b[0m");
#endif
}

char *my_asprintf(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int len = vsnprintf(NULL, 0, fmt, ap);
  char *res = malloc(len + 1);
  va_end(ap);
  va_start(ap, fmt);
  vsnprintf(res, len + 1, fmt, ap);
  va_end(ap);
  return res;
}

char *my_vasprintf(const char *fmt, va_list ap) {
  // vsnprintf may mess up its va_list parameter, so make a copy
  va_list ap2;
  va_copy(ap2, ap);
  int len = vsnprintf(NULL, 0, fmt, ap2);
  char *res = malloc(len + 1);
  vsnprintf(res, len + 1, fmt, ap);
  return res;
}

typedef struct {
  void **lib_ptr;
  int lib_len;
  const char *error;
} LibraryList;

void *my_dlopen(int files_len, const char **files_ptr) {
  LibraryList *list = malloc(sizeof(LibraryList));
  list->lib_len = files_len;
  list->lib_ptr = malloc(sizeof(void*) * files_len);
  list->error = NULL;
  for (int i = 0; i < files_len; i++) {
    list->lib_ptr[i] = dlopen(files_ptr[i], RTLD_LAZY);
    if (!list->lib_ptr[i]) {
      fprintf(stderr, "dlopen failed: %s\n", dlerror());
      abort();
    }
  }
  return list;
}

void *my_dlsym(void *ptr, const char *symbol) {
  LibraryList *list = (LibraryList*) ptr;
  for (int i = 0; i < list->lib_len; i++) {
    void *sym = dlsym(list->lib_ptr[i], symbol);
    // fprintf(stderr, "dlsym %i try load %s => %p\n", i, symbol, sym);
    if (sym) return sym;
  }
  list->error = my_asprintf("Symbol %s not found.", symbol);
  return NULL;
}

const char *my_dlerror(void *ptr) {
  LibraryList *list = (LibraryList*) ptr;
  const char *res = list->error;
  list->error = NULL;
  return res;
}
