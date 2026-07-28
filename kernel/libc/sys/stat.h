/* freestanding sys/stat.h for the LarzOS kernel (no filesystem yet) */
#ifndef _LARZOS_SYS_STAT_H
#define _LARZOS_SYS_STAT_H
#include <stddef.h>
typedef unsigned mode_t;
struct stat { mode_t st_mode; unsigned long st_size; };
#define S_IFMT  0170000
#define S_IFREG 0100000
#define S_IFDIR 0040000
#define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)
#define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)
int stat(const char *path, struct stat *buf);
int mkdir(const char *path, mode_t mode);
#endif
