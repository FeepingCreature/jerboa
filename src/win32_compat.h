#ifndef JERBOA_WIN32_COMPAT_H
#define JERBOA_WIN32_COMPAT_H

#include <stdlib.h>

#ifdef _WIN32
#include <malloc.h>
#else
#include <alloca.h>
#endif

#ifdef _WIN32

#include <time.h>

#define CLOCK_MONOTONIC 0

int clock_gettime(int dummy, struct timespec *ct);

void bzero(void *s, size_t n);

void *memmem(const void *haystack, size_t hay_len, const void *needle, size_t needle_len);

char *get_current_dir_name();

#define RTLD_LAZY 0

void *dlopen(const char *path, int mode);

void *dlsym(void *handle, const char *symbol);

char *dlerror();

#else

#include <dlfcn.h>

#endif

#endif
