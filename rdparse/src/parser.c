#include "rdparse/parser.h"
#include "rdparse/util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <assert.h>
#include <stdbool.h>

#include <unicode/utf8.h>
#include <unicode/uchar.h>

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
  if ((text[0] >= 'a' && text[0] <= 'z') || (text[0] >= 'A' && text[0] <= 'Z') || text[0] == '_') text++;
  else if ((unsigned char)text[0] > 0x7f) {
    UChar32 cp;
    int i = 0;
    U8_NEXT_UNSAFE(text, i, cp);
    text += i;
    if (!u_isalpha(cp)) return NULL;
  }
  else return NULL;
  while (text[0]) {
    if ((text[0] >= 'a' && text[0] <= 'z') || (text[0] >= 'A' && text[0] <= 'Z') || (text[0] >= '0' && text[0] <= '9') || text[0] == '_') text++;
    else if ((unsigned char)text[0] > 0x7f) {
      UChar32 cp;
      int i = 0;
      U8_NEXT_UNSAFE(text, i, cp);
      if (!u_isalnum(cp)) break;
      text += i;
    }
    else break;
  }
  
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
  
  // TODO extend
  if (strcmp(res, "new") == 0) {
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
  if (text[0] != '"' && text[0] != '\'') return PARSE_NONE;
  char marker = text[0];
  text++;
  int len_escaped = 0;
  while (text[0] && text[0] != marker) {
    if (marker == '"' && text[0] == '\\') {
      text++;
      if (!text[0]) {
        log_parser_error(text, "unterminated escape");
        return PARSE_ERROR;
      }
    }
    len_escaped ++;
    text++;
  }
  if (!text[0]) {
    log_parser_error(text, "closing quote mark is missing");
    return PARSE_ERROR;
  }
  text++;
  
  char *scan = start + 1;
  char *res = malloc(len_escaped + 1);
  for (int i = 0; i < len_escaped; scan++, i++) {
    if (marker == '"' && scan[0] == '\\') {
      scan++;
      switch (scan[0]) {
        case '"': res[i] = '"'; break;
        case '\\': res[i] = '\\'; break;
        case 'n': res[i] = '\n'; break;
        case 'r': res[i] = '\r'; break;
        case 't': res[i] = '\t'; break;
        default:
          log_parser_error(scan, "unknown escape sequence");
          return PARSE_ERROR;
      }
      continue;
    }
    res[i] = scan[0];
  }
  res[len_escaped] = 0;
  
  *textp = text;
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
  const char *file, *fn;
  int row, col;
  if (find_text_pos(location, &file, &fn, &line, &row, &col)) {
    fprintf(stderr, "\x1b[1m%s:%i:%i: \x1b[31merror:\x1b[0m ", file, row + 1, col + 1);
    
    va_list ap;
    va_start(ap, format);
    vfprintf(stderr, format, ap);
    va_end(ap);
    
    fprintf(stderr, "\n");
    if (*(line.end - 1) == '\n') line.end --;
    fprintf(stderr, "%.*s\n", (int) (line.end - line.start), line.start);
    int utf8_col = utf8_strnlen(line.start, col);
    int utf8_line_len = utf8_strnlen(line.start, line.end - line.start);
    for (int i = 0; i <= utf8_line_len; ++i) {
      if (i < utf8_col) fprintf(stderr, " ");
      else if (i == utf8_col) fprintf(stderr, "\x1b[1m\x1b[32m^\x1b[0m");
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
