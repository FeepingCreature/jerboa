#include "parser.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdbool.h>

static bool starts_with(char **textp, char *cmp) {
  char *text = *textp;
  while (*cmp) {
    if (!text[0]) return false;
    if (text[0] != cmp[0]) return false;
    text++;
    cmp++;
  }
  *textp = text;
  return true;
}

void eat_filler(char **textp) {
  int comment_depth = 0;
  while (**textp) {
    if (comment_depth) {
      if (starts_with(textp, "*/")) comment_depth --;
      else (*textp)++;
    } else {
      if (starts_with(textp, "/*")) comment_depth ++;
      else if (starts_with(textp, "//")) {
        while (**textp && **textp != '\n') (*textp)++;
      }
      else if (**textp == ' ' || **textp == '\n') (*textp)++;
      else break;
    }
  }
}

bool eat_string(char **textp, char *keyword) {
  char *text = *textp;
  eat_filler(&text);
  if (starts_with(&text, keyword)) {
    *textp = text;
    return true;
  }
  return false;
}

char *parse_identifier_all(char **textp) {
  char *text = *textp;
  eat_filler(&text);
  char *start = text;
  if (text[0] && ((text[0] >= 'a' && text[0] <= 'z') || (text[0] >= 'A' && text[0] <= 'Z') || text[0] == '_')) text++;
  else return NULL;
  while (text[0] && ((text[0] >= 'a' && text[0] <= 'z') || (text[0] >= 'A' && text[0] <= 'Z') || (text[0] >= '0' && text[0] <= '9') || text[0] == '_')) text++;
  
  int len = text - start;
  char *res = malloc(len + 1);
  memcpy(res, start, len);
  res[len] = 0;
  
  *textp = text;
  return res;
}

char *parse_identifier(char **textp) {
  char *text = *textp;
  char *res = parse_identifier_all(&text);
  if (res == NULL) return res;
  
  if (strncmp(res, "function", 8) == 0 || strncmp(res, "method", 6) == 0 || strncmp(res, "new", 3) == 0) {
    // reserved identifier
    free(res);
    return NULL;
  }
  
  *textp = text;
  return res;
}

bool parse_int(char **textp, int *outp) {
  char *text = *textp;
  eat_filler(&text);
  char *start = text;
  int base = 10;
  if (text[0] && text[0] == '-') text++;
  if (text[0] == '0' && text[1] == 'x') {
    base = 16;
    text += 2;
  }
  while (text[0]) {
    if (base >= 10 && text[0] >= '0' && text[0] <= '9') text++;
    else if (base >= 16 && ((text[0] >= 'A' && text[0] <= 'F') || (text[0] >= 'a' && text[0] <= 'f'))) text++;
    else break;
  }
  if (text == start) return false;
  
  *textp = text;
  int len = text - start;
  char *res = malloc(len + 1);
  memcpy(res, start, len);
  res[len] = 0;
  *outp = strtol(res, NULL, base);
  free(res);
  return true;
}

bool parse_float(char **textp, float *outp) {
  char *text = *textp;
  eat_filler(&text);
  char *start = text;
  if (text[0] && text[0] == '-') text++;
  while (text[0] && text[0] >= '0' && text[0] <= '9') text++;
  // has to be at least a . in there, or else it's an int
  if (!text[0] || text[0] != '.') return false;
  text++;
  while (text[0] && text[0] >= '0' && text[0] <= '9') text++;
  
  *textp = text;
  int len = text - start;
  char *res = malloc(len + 1);
  memcpy(res, start, len);
  res[len] = 0;
  *outp = atof(res);
  free(res);
  return true;
}

bool parse_string(char **textp, char **outp) {
  char *text = *textp;
  eat_filler(&text);
  char *start = text;
  if (text[0] != '"') return false;
  text++;
  while (text[0] != '"') text++; // TODO escape
  text++;
  
  *textp = text;
  int len = text - start;
  char *res = malloc(len - 2 + 1);
  memcpy(res, start + 1, len - 2);
  res[len - 2] = 0;
  *outp = res;
  return true;
}

bool eat_keyword(char **textp, char *keyword) {
  char *text = *textp;
  char *cmp = parse_identifier_all(&text);
  if (!cmp || strcmp(cmp, keyword) != 0) return false;
  *textp = text;
  return true;
}

void log_parser_error(char *location, char *format, ...) {
  fprintf(stderr, "at %.*s:\n", 20, location);
  fprintf(stderr, "parser error: ");
  va_list ap;
  va_start(ap, format);
  vfprintf(stderr, format, ap);
  va_end(ap);
  fprintf(stderr, "\n");
}
