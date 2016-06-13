#ifndef UTIL_H
#define UTIL_H

typedef struct {
  char *ptr;
  int len;
} String;

String readfile(char *filename);

#endif
