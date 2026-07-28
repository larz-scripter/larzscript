/* freestanding unistd.h for the LarzOS kernel (OS calls are stubbed) */
#ifndef _LARZOS_UNISTD_H
#define _LARZOS_UNISTD_H
#include <stddef.h>
typedef unsigned useconds_t;
char *getcwd(char *buf, size_t size);
int   chdir(const char *path);
int   rmdir(const char *path);
int   unlink(const char *path);
int   access(const char *path, int mode);
int   usleep(unsigned usec);
unsigned sleep(unsigned seconds);
#endif
