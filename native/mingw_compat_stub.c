/* Windows-only. MSYS2's prebuilt mingw-w64 libssh.a (built with MSYS2's
 * own, newer mingw-w64-crt) references a few functions that Ubuntu's
 * apt-installed gcc-mingw-w64-x86-64 cross-compiler's older bundled
 * runtime doesn't provide - a toolchain-version mismatch between "compile
 * larzscript.c with Ubuntu's cross-gcc" and "link against a library built
 * with MSYS2's own gcc", not a real platform limitation. Real, correct
 * implementations here, not arbitrary placeholders - same standard as
 * native/gssapi_stub.c. */
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <sys/stat.h>

/* libssh's precompiled object expects the non-underscore-prefixed name;
 * mingw-w64's real, standard exported symbol is the underscore-prefixed
 * _fstat64i32 (Microsoft CRT naming convention) - same function, same
 * struct, just an alias for the name libssh was built against. */
int fstat64i32(int fd, struct _stat64i32 *buf) {
  return _fstat64i32(fd, buf);
}

/* The whole point of memset_explicit (vs plain memset) is that it's
 * guaranteed to actually happen even when the compiler could otherwise
 * prove the write is dead (the buffer going out of scope right after) -
 * routing through a volatile function pointer is the standard technique
 * for this, since the compiler can't assume what a call through a
 * volatile pointer does or optimize it away. */
static void *(*volatile memset_volatile)(void *, int, size_t) = memset;
void memset_explicit(void *s, int c, size_t n) {
  memset_volatile(s, c, n);
}

char *strndup(const char *s, size_t n) {
  size_t len = 0;
  while (len < n && s[len]) len++;
  char *out = malloc(len + 1);
  if (!out) return NULL;
  memcpy(out, s, len);
  out[len] = 0;
  return out;
}

int isblank(int c) {
  return c == ' ' || c == '\t';
}
