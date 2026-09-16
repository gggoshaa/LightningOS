#ifndef _LOS_USERS_H
#define _LOS_USERS_H

#include "types.h"

#define USER_NAME_MAX 32
#define USER_HOME_MAX 64
#define USER_MAX      8
#define PASSWORD_MIN  4
#define PASSWORD_MAX  64

typedef struct {
    char     name[USER_NAME_MAX];
    char     home[USER_HOME_MAX];
    uint32_t uid;
    uint32_t gid;
    uint32_t pw_hash;
    bool     active;
} user_t;

void    users_init(void);
bool    users_setup_needed(void);
void    users_setup_wizard(void);   /* first boot: creates root and friends */
void    users_login(void);          /* loops until someone authenticates    */
void    users_logout(void);

user_t *users_current(void);
bool    users_is_root(void);
user_t *users_find(const char *name);
user_t *users_at(int index);
int     users_count(void);

/* All four return 0 on success and a negative value on failure. */
int users_add(const char *name, const char *password, bool admin);
int users_delete(const char *name);
int users_set_password(const char *name, const char *password);
int users_switch(const char *name, const char *password);

bool users_check_password(const user_t *user, const char *password);

/* Restores an account verbatim from a saved snapshot, hash included. */
int users_import(const char *name, const char *home,
                 uint32_t uid, uint32_t gid, uint32_t pw_hash);

/* True when `path` is inside the current user's own writable area. */
bool users_may_write(const char *abspath);

#endif
