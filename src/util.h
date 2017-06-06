#ifndef JERBOA_UTIL_H
#define JERBOA_UTIL_H

#include "win32_compat.h"

#include <time.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdbool.h>

long long get_clock_and_difference(struct timespec *target_clock, struct timespec *compare_clock);

void format_bold(FILE *output);

void format_reset(FILE *output);

char *my_asprintf(const char *fmt, ...);

char *my_vasprintf(const char *fmt, va_list ap);

// open multiple libraries "at once"
void *my_dlopen(int files_len, const char **files_ptr);

void *my_dlsym(void *ptr, const char *symbol);

const char *my_dlerror(void *ptr);

// if seeded with 1, does not repeat until it hits 1 again (see lcgtest.c) at 2^31.
// This SHOULD be sufficient.
uint32_t lcg_parkmiller(uint32_t a);

#endif
