#include "fs.h"
#include "mem.h"
#include "string.h"

/* A tiny in-memory filesystem: every node is a heap allocation and directories
   keep their children in a singly linked list. Enough to give the shell a
   Unix-shaped namespace without touching a disk. */

static fs_node_t *root_node;
static fs_node_t *current_dir;
static int        node_count;

static void free_subtree(fs_node_t *node);

static fs_node_t *node_new(const char *name, fs_type_t type, fs_node_t *parent)
{
    fs_node_t *node = (fs_node_t *)kcalloc(1, sizeof(fs_node_t));

    if (!node)
        return NULL;

    strncpy(node->name, name, FS_NAME_MAX - 1);
    node->type   = type;
    node->parent = parent;

    if (parent) {
        /* Append so that listings keep creation order. */
        if (!parent->children) {
            parent->children = node;
        } else {
            fs_node_t *last = parent->children;
            while (last->next)
                last = last->next;
            last->next = node;
        }
    }
    node_count++;
    return node;
}

static fs_node_t *find_child(fs_node_t *dir, const char *name)
{
    if (!dir || dir->type != FS_DIR)
        return NULL;

    for (fs_node_t *child = dir->children; child; child = child->next) {
        if (strcmp(child->name, name) == 0)
            return child;
    }
    return NULL;
}

/* Walks `path`. When `stop_before_last` is set the final component is not
   followed; it is copied into `last` instead so callers can create it. */
static fs_node_t *walk(const char *path, bool stop_before_last, char *last)
{
    char buffer[FS_PATH_MAX];
    fs_node_t *node;
    int i = 0;

    if (!path || !*path)
        return NULL;

    strncpy(buffer, path, FS_PATH_MAX - 1);
    buffer[FS_PATH_MAX - 1] = '\0';

    node = (buffer[0] == '/') ? root_node : current_dir;
    if (buffer[0] == '/')
        i = 1;

    if (last)
        last[0] = '\0';

    while (buffer[i]) {
        char component[FS_NAME_MAX];
        int len = 0;

        while (buffer[i] == '/')
            i++;
        if (!buffer[i])
            break;

        while (buffer[i] && buffer[i] != '/' && len < FS_NAME_MAX - 1)
            component[len++] = buffer[i++];
        component[len] = '\0';
        while (buffer[i] && buffer[i] != '/')
            i++;                         /* skip an over-long component tail */

        /* Is this the last component? */
        int j = i;
        while (buffer[j] == '/')
            j++;
        bool is_last = (buffer[j] == '\0');

        if (is_last && stop_before_last) {
            if (last)
                strncpy(last, component, FS_NAME_MAX - 1);
            return node;
        }

        if (strcmp(component, ".") == 0)
            continue;
        if (strcmp(component, "..") == 0) {
            node = node->parent ? node->parent : root_node;
            continue;
        }

        node = find_child(node, component);
        if (!node)
            return NULL;
    }
    return node;
}

/* Frees the whole tree and starts again with an empty root. Used when a
   snapshot is restored from disk over the built-in skeleton. */
void fs_reset(void)
{
    if (root_node) {
        fs_node_t *child = root_node->children;

        while (child) {
            fs_node_t *next = child->next;
            free_subtree(child);
            child = next;
        }
        root_node->children = NULL;
    } else {
        root_node = node_new("/", FS_DIR, NULL);
    }
    current_dir = root_node;
}

void fs_init(void)
{
    root_node = node_new("/", FS_DIR, NULL);
    current_dir = root_node;

    fs_create("/bin", FS_DIR);
    fs_create("/etc", FS_DIR);
    fs_create("/dev", FS_DIR);
    fs_create("/tmp", FS_DIR);
    fs_create("/home", FS_DIR);
    fs_create("/root", FS_DIR);

    fs_node_t *file;

    file = fs_create("/etc/motd", FS_FILE);
    fs_write(file,
             "Welcome to LightningOS.\n"
             "32-bit protected mode, preemptive tasks, ATA disk, no paging yet.\n"
             "Type 'help' to see what the shell can do.\n", false);

    file = fs_create("/etc/hostname", FS_FILE);
    fs_write(file, "lightning\n", false);

    /* /etc/passwd is generated from the account table once users exist. */
    fs_create("/etc/passwd", FS_FILE);
}

fs_node_t *fs_root(void) { return root_node; }
fs_node_t *fs_cwd(void)  { return current_dir; }

void fs_set_cwd(fs_node_t *dir)
{
    if (dir && dir->type == FS_DIR)
        current_dir = dir;
}

fs_node_t *fs_resolve(const char *path)
{
    if (!path || !*path)
        return current_dir;
    if (strcmp(path, "/") == 0)
        return root_node;
    return walk(path, false, NULL);
}

fs_node_t *fs_create(const char *path, fs_type_t type)
{
    char name[FS_NAME_MAX];
    fs_node_t *parent = walk(path, true, name);

    if (!parent || parent->type != FS_DIR || !name[0])
        return NULL;
    if (find_child(parent, name))
        return NULL;                     /* already exists */

    return node_new(name, type, parent);
}

static void free_subtree(fs_node_t *node)
{
    fs_node_t *child = node->children;

    while (child) {
        fs_node_t *next = child->next;
        free_subtree(child);
        child = next;
    }
    if (node->data)
        kfree(node->data);
    kfree(node);
    node_count--;
}

int fs_remove(const char *path)
{
    fs_node_t *node = fs_resolve(path);

    if (!node || node == root_node)
        return -1;
    if (node == current_dir)
        return -2;                       /* refuse to remove the cwd */

    fs_node_t *parent = node->parent;
    if (!parent)
        return -1;

    if (parent->children == node) {
        parent->children = node->next;
    } else {
        fs_node_t *prev = parent->children;
        while (prev && prev->next != node)
            prev = prev->next;
        if (!prev)
            return -1;
        prev->next = node->next;
    }

    free_subtree(node);
    return 0;
}

int fs_write(fs_node_t *file, const char *text, bool append)
{
    if (!text)
        return -1;
    return fs_write_n(file, text, strlen(text), append);
}

int fs_write_n(fs_node_t *file, const char *bytes, size_t add, bool append)
{
    if (!file || file->type != FS_FILE || !bytes)
        return -1;

    size_t base = append ? file->size : 0;
    size_t needed = base + add + 1;

    if (needed > file->capacity) {
        size_t capacity = needed + 64;
        char *buffer = (char *)kmalloc(capacity);

        if (!buffer)
            return -1;
        if (base && file->data)
            memcpy(buffer, file->data, base);
        if (file->data)
            kfree(file->data);
        file->data = buffer;
        file->capacity = capacity;
    }

    memcpy(file->data + base, bytes, add);
    file->size = base + add;
    file->data[file->size] = '\0';
    return 0;
}

void fs_abspath(fs_node_t *node, char *out, size_t size)
{
    const char *parts[32];
    int depth = 0;

    if (!node || size == 0)
        return;

    for (fs_node_t *n = node; n && n->parent && depth < 32; n = n->parent)
        parts[depth++] = n->name;

    out[0] = '\0';
    if (depth == 0) {
        strncpy(out, "/", size - 1);
        out[size - 1] = '\0';
        return;
    }

    size_t pos = 0;
    for (int i = depth - 1; i >= 0; i--) {
        size_t len = strlen(parts[i]);
        if (pos + len + 2 >= size)
            break;
        out[pos++] = '/';
        memcpy(out + pos, parts[i], len);
        pos += len;
    }
    out[pos] = '\0';
}

int fs_count_nodes(void) { return node_count; }
