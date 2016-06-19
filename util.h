#ifndef UTIL_H
#define UTIL_H

typedef struct {
  char *start, *end;
} TextRange;

TextRange readfile(char *filename);

#endif
