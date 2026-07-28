/* freestanding dirent.h for the LarzOS kernel (no filesystem yet) */
#ifndef _LARZOS_DIRENT_H
#define _LARZOS_DIRENT_H
typedef struct _LZ_DIR DIR;
struct dirent { char d_name[256]; };
DIR *opendir(const char *name);
struct dirent *readdir(DIR *dirp);
int closedir(DIR *dirp);
#endif
