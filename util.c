#include "util.h"

#include <fcntl.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <unistd.h>
#include <stdbool.h>

TextRange readfile(char *filename) {
  int file = open(filename, O_RDONLY);
  if (file == -1) { fprintf(stderr, "cannot open file '%s': %s\n", filename, strerror(errno)); assert(false); }
  char *res_ptr = NULL; int res_len = 0;
  int bytes_read = 0;
  do {
    res_len = bytes_read + 1024;
    res_ptr = realloc(res_ptr, res_len);
    ssize_t numitems = read(file, res_ptr + bytes_read, res_len - bytes_read - 1);
    if (numitems == -1) { fprintf(stderr, "cannot read from file: %s\n", strerror(errno)); assert(false); }
    if (numitems == 0) {
      res_ptr[bytes_read] = 0;
      return (TextRange){res_ptr, res_ptr + bytes_read};
    }
    bytes_read += numitems;
  } while (true);
}

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
