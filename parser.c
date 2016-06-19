#include "parser.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <assert.h>
#include <stdbool.h>

struct _FileRecord;
typedef struct _FileRecord FileRecord;

struct _FileRecord {
  FileRecord *prev;
  TextRange text;
  const char *name;
  int row_start, col_start;
};

// TODO mutex the shit out of this
FileRecord *record = NULL;

void register_file(TextRange text, const char *name, int row_start, int col_start) {
  FileRecord *newrecord = malloc(sizeof(FileRecord));
  newrecord->prev = record;
  newrecord->text = text;
  newrecord->name = name;
  newrecord->row_start = row_start;
  newrecord->col_start = col_start;
  record = newrecord;
}

bool find_text_pos(char *text, const char **name_p, TextRange *line_p, int *row_p, int *col_p) {
  FileRecord *rec = record;
  while (rec) {
    if (text >= rec->text.start && text < rec->text.end) {
      *name_p = rec->name;
      
      int row_nr = 0;
      TextRange line = (TextRange) { rec->text.start, rec->text.start };
      while (line.start < rec->text.end) {
        while (line.end < rec->text.end && *line.end != '\n') line.end ++; // scan to newline
        if (line.end < rec->text.end) line.end ++; // scan past newline
        if (text >= line.start && text < line.end) {
          int col_nr = text - line.start;
          *line_p = line;
          *row_p = row_nr + rec->row_start;
          *col_p = col_nr + ((row_nr == 0) ? rec->col_start : 0);
          return true;
        }
        line.start = line.end;
        row_nr ++;
      }
      assert(false); // logic error, wtf - text in range but not in any line??
    }
    rec = rec->prev;
  }
  return false;
}

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

ParseResult parse_string(char **textp, char **outp) {
  char *text = *textp;
  eat_filler(&text);
  char *start = text;
  if (text[0] != '"') return PARSE_NONE;
  text++;
  while (text[0] && text[0] != '"') text++; // TODO escape
  if (!text[0]) {
    log_parser_error(text, "closing quote mark is missing");
    return PARSE_ERROR;
  }
  text++;
  
  *textp = text;
  int len = text - start;
  char *res = malloc(len - 2 + 1);
  memcpy(res, start + 1, len - 2);
  res[len - 2] = 0;
  *outp = res;
  return PARSE_OK;
}

bool eat_keyword(char **textp, char *keyword) {
  char *text = *textp;
  char *cmp = parse_identifier_all(&text);
  if (!cmp || strcmp(cmp, keyword) != 0) return false;
  *textp = text;
  return true;
}

void log_parser_error(char *location, char *format, ...) {
  TextRange line;
  const char *file;
  int row, col;
  if (find_text_pos(location, &file, &line, &row, &col)) {
    fprintf(stderr, "\x1b[1m%s:%i:%i: \x1b[31merror:\x1b[0m ", file, row + 1, col + 1);
    
    va_list ap;
    va_start(ap, format);
    vfprintf(stderr, format, ap);
    va_end(ap);
    
    fprintf(stderr, "\n");
    fprintf(stderr, "%.*s", (int) (line.end - line.start), line.start);
    for (int i = 0; i < line.end - line.start; ++i) {
      if (i < col) fprintf(stderr, " ");
      else if (i == col) fprintf(stderr, "\x1b[1m\x1b[32m^\x1b[0m");
    }
    fprintf(stderr, "\n");
  } else {
    fprintf(stderr, "parser error:\n");
    fprintf(stderr, "at %.*s:\n", 20, location);
    va_list ap;
    va_start(ap, format);
    vfprintf(stderr, format, ap);
    va_end(ap);
    fprintf(stderr, "\n");
  }
}
