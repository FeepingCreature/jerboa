#ifndef JERBOA_UTIL_H
#define JERBOA_UTIL_H

#include "win32_compat.h"

#include <time.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdarg.h>

typedef struct {
  char *start, *end;
} TextRange;

typedef struct {
  const char *file;
  TextRange range;
} FileEntry;

TextRange readfile(char *filename);

bool file_exists(char *filename);

void register_file(TextRange text, const char *name, int row_start, int col_start);

void register_function(TextRange text, const char *name);

size_t utf8_strnlen(const char *ptr, size_t length);

size_t utf8_strlen(const char *ptr);

FileEntry *get_files(int *length_p);

void utf8_step(const char **ptr, int num, const char **error_p);

bool find_text_pos(char *text, const char **name_p, const char **function_p, TextRange *line_p, int *row_p, int *col_p);

long long get_clock_and_difference(struct timespec *target_clock, struct timespec *compare_clock);

void format_bold(FILE *output);

void format_reset(FILE *output);

char *my_asprintf(const char *fmt, ...);

char *my_vasprintf(const char *fmt, va_list ap);

// open multiple libraries "at once"
void *my_dlopen(int files_len, const char **files_ptr);

void *my_dlsym(void *ptr, const char *symbol);

const char *my_dlerror(void *ptr);

#endif
