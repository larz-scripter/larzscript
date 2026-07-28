/* freestanding stdio.h for the LarzOS kernel - just what larzscript.c uses */
#ifndef _LARZOS_STDIO_H
#define _LARZOS_STDIO_H
#include <stddef.h>
#include <stdarg.h>

typedef struct _LZ_FILE FILE;
extern FILE *stdin, *stdout, *stderr;

#define EOF (-1)

int   printf(const char *fmt, ...);
int   fprintf(FILE *stream, const char *fmt, ...);
int   snprintf(char *buf, size_t n, const char *fmt, ...);
int   vsnprintf(char *buf, size_t n, const char *fmt, va_list ap);
int   putchar(int c);
int   puts(const char *s);
int   fputs(const char *s, FILE *stream);
int   fputc(int c, FILE *stream);
int   fflush(FILE *stream);

FILE *fopen(const char *path, const char *mode);
int   fclose(FILE *stream);
size_t fread(void *ptr, size_t size, size_t nmemb, FILE *stream);
size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream);
char *fgets(char *s, int size, FILE *stream);

FILE *popen(const char *cmd, const char *mode);
int   pclose(FILE *stream);

int   rename(const char *oldp, const char *newp);
int   remove(const char *path);

#endif
