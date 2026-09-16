#ifndef _LOS_FS_H
#define _LOS_FS_H

#include "types.h"

#define FS_NAME_MAX 32
#define FS_PATH_MAX 256

typedef enum { FS_FILE = 1, FS_DIR = 2 } fs_type_t;

typedef struct fs_node {
    char             name[FS_NAME_MAX];
    fs_type_t        type;
    char            *data;          /* file contents (NUL terminated) */
    size_t           size;
    size_t           capacity;
    struct fs_node  *parent;
    struct fs_node  *children;      /* first child, directories only */
    struct fs_node  *next;          /* next sibling */
} fs_node_t;

void       fs_init(void);
fs_node_t *fs_root(void);
fs_node_t *fs_cwd(void);
void       fs_set_cwd(fs_node_t *dir);

fs_node_t *fs_resolve(const char *path);
fs_node_t *fs_create(const char *path, fs_type_t type);
int        fs_remove(const char *path);
int        fs_write(fs_node_t *file, const char *text, bool append);
void       fs_abspath(fs_node_t *node, char *out, size_t size);
int        fs_count_nodes(void);

#endif
