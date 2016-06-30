#ifndef UTIL_H
#define UTIL_H

#include <time.h>
#include <stdbool.h>

typedef struct {
  char *start, *end;
} TextRange;

TextRange readfile(char *filename);

void register_file(TextRange text, const char *name, int row_start, int col_start);

bool find_text_pos(char *text, const char **name_p, TextRange *line_p, int *row_p, int *col_p);

long long get_clock_and_difference(struct timespec *target_clock, struct timespec *compare_clock);

char *my_asprintf(char *fmt, ...);

#endif
