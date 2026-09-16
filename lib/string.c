#include "string.h"

void *memset(void *dst, int value, size_t count)
{
    uint8_t *p = (uint8_t *)dst;
    while (count--)
        *p++ = (uint8_t)value;
    return dst;
}

void *memcpy(void *dst, const void *src, size_t count)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (count--)
        *d++ = *s++;
    return dst;
}

void *memmove(void *dst, const void *src, size_t count)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;

    if (d == s || count == 0)
        return dst;

    if (d < s) {
        while (count--)
            *d++ = *s++;
    } else {
        d += count;
        s += count;
        while (count--)
            *--d = *--s;
    }
    return dst;
}

int memcmp(const void *a, const void *b, size_t count)
{
    const uint8_t *x = (const uint8_t *)a;
    const uint8_t *y = (const uint8_t *)b;
    while (count--) {
        if (*x != *y)
            return (int)*x - (int)*y;
        x++; y++;
    }
    return 0;
}

size_t strlen(const char *s)
{
    size_t n = 0;
    while (s[n])
        n++;
    return n;
}

int strcmp(const char *a, const char *b)
{
    while (*a && (*a == *b)) {
        a++; b++;
    }
    return (int)(uint8_t)*a - (int)(uint8_t)*b;
}

int strncmp(const char *a, const char *b, size_t n)
{
    while (n && *a && (*a == *b)) {
        a++; b++; n--;
    }
    if (n == 0)
        return 0;
    return (int)(uint8_t)*a - (int)(uint8_t)*b;
}

char *strcpy(char *dst, const char *src)
{
    char *out = dst;
    while ((*dst++ = *src++))
        ;
    return out;
}

char *strncpy(char *dst, const char *src, size_t n)
{
    size_t i = 0;
    for (; i < n && src[i]; i++)
        dst[i] = src[i];
    for (; i < n; i++)
        dst[i] = '\0';
    return dst;
}

char *strcat(char *dst, const char *src)
{
    char *out = dst;
    while (*dst)
        dst++;
    while ((*dst++ = *src++))
        ;
    return out;
}

char *strchr(const char *s, int c)
{
    for (; *s; s++) {
        if (*s == (char)c)
            return (char *)s;
    }
    return (c == 0) ? (char *)s : NULL;
}

char *strrchr(const char *s, int c)
{
    const char *found = NULL;
    for (; *s; s++) {
        if (*s == (char)c)
            found = s;
    }
    return (char *)found;
}

static int is_space(char c)
{
    return c == ' ' || c == '\t';
}

int str_split(char *line, char **argv, int max_args)
{
    int argc = 0;

    while (*line && argc < max_args) {
        while (is_space(*line))
            *line++ = '\0';
        if (!*line)
            break;
        argv[argc++] = line;
        while (*line && !is_space(*line))
            line++;
    }
    return argc;
}
