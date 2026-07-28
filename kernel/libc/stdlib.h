/* freestanding stdlib.h for the LarzOS kernel */
#ifndef _LARZOS_STDLIB_H
#define _LARZOS_STDLIB_H
#include <stddef.h>

void *malloc(size_t size);
void *calloc(size_t nmemb, size_t size);
void *realloc(void *ptr, size_t size);
void  free(void *ptr);

void  exit(int status);
void  abort(void);

char *getenv(const char *name);
int   system(const char *cmd);

double strtod(const char *nptr, char **endptr);
long   strtol(const char *nptr, char **endptr, int base);
int    atoi(const char *nptr);

void  qsort(void *base, size_t nmemb, size_t size,
            int (*cmp)(const void *, const void *));

char *realpath(const char *path, char *resolved);

#endif
