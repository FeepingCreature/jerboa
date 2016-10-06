#include "win32_compat.h"

#include "util.h"

#include <stdbool.h>
#include <string.h>

#ifdef _WIN32

#include <direct.h>
#include <stdio.h>
#include <windows.h>

// thanks http://stackoverflow.com/questions/5404277/porting-clock-gettime-to-windows

static bool init_gettime = false;
static LARGE_INTEGER qpc_per_sec;

int clock_gettime(int dummy, struct timespec *ct) {
  if (!init_gettime) {
    init_gettime = true;
    if (QueryPerformanceFrequency(&qpc_per_sec) == 0) {
      qpc_per_sec.QuadPart = 0;
    }
  }
  
  LARGE_INTEGER count;
  if (!ct || qpc_per_sec.QuadPart <= 0 || QueryPerformanceCounter(&count) == 0) {
    return -1;
  }
  
  ct->tv_sec = count.QuadPart / qpc_per_sec.QuadPart;
  ct->tv_nsec = ((count.QuadPart % qpc_per_sec.QuadPart) * 1000000000) / qpc_per_sec.QuadPart;
  
  return 0;
}

void bzero(void *s, size_t n) {
  memset(s, 0, n);
}

// thanks https://lists.gnu.org/archive/html/qemu-devel/2015-05/msg03416.html
void *memmem(const void *haystack, size_t hay_len, const void *needle, size_t needle_len) {
  if (needle_len == 0) return (void*) haystack;
  if (hay_len < needle_len) return NULL;
  const char *hay = (const char*) haystack;
  const void *hay_last = hay + (hay_len - needle_len);
  for (; hay != hay_last; hay++) {
    if (memcmp(hay, needle, needle_len) == 0) {
      return (void*) hay;
    }
  }
  return NULL;
}

char *get_current_dir_name() {
  char *buffer = _getcwd(NULL, 0);
  if (!buffer) {
    fprintf(stderr, "cannot get current directory");
    abort();
  }
  return buffer;
}

void *dlopen(const char *path, int flags) {
  return LoadLibrary(path);
}

void *dlsym(void *handle, const char *symbol) {
  FARPROC addr = GetProcAddress(handle, symbol);
  return *(void**) &addr;
}

char *dlerror() {
  return my_asprintf("win32 error %i", GetLastError());;
}

#endif
