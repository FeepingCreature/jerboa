#ifndef RDPARSE_UTIL_H
#define RDPARSE_UTIL_H

#include <stddef.h>
#include <stdlib.h>
#include <stdbool.h>

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

#endif
