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

// taken from https://en.wikipedia.org/wiki/Lehmer_random_number_generator
uint32_t lcg_parkmiller(uint32_t a) {
    return ((uint64_t)a * 48271UL) % 2147483647UL;
}
