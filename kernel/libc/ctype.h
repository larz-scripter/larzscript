/* freestanding ctype.h for the LarzOS kernel */
#ifndef _LARZOS_CTYPE_H
#define _LARZOS_CTYPE_H
int isdigit(int c);
int isspace(int c);
int isalpha(int c);
int isalnum(int c);
int isupper(int c);
int islower(int c);
int isprint(int c);
int toupper(int c);
int tolower(int c);
#endif
