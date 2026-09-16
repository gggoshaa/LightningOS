#ifndef _LOS_STRING_H
#define _LOS_STRING_H

#include "types.h"

void  *memset(void *dst, int value, size_t count);
void  *memcpy(void *dst, const void *src, size_t count);
void  *memmove(void *dst, const void *src, size_t count);
int    memcmp(const void *a, const void *b, size_t count);

size_t strlen(const char *s);
int    strcmp(const char *a, const char *b);
int    strncmp(const char *a, const char *b, size_t n);
char  *strcpy(char *dst, const char *src);
char  *strncpy(char *dst, const char *src, size_t n);
char  *strcat(char *dst, const char *src);
char  *strchr(const char *s, int c);
char  *strrchr(const char *s, int c);

/* Splits `line` in place on runs of spaces/tabs. Returns the token count. */
int    str_split(char *line, char **argv, int max_args);

#endif
