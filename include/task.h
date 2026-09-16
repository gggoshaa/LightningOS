#ifndef _LOS_TASK_H
#define _LOS_TASK_H

#include "types.h"

#define TASK_NAME_MAX  24
#define TASK_MAX       16
#define TASK_STACK_SIZE 16384

typedef enum {
    TASK_UNUSED = 0,
    TASK_READY,         /* runnable, waiting for its turn */
    TASK_SLEEPING,      /* waiting for a tick deadline    */
    TASK_DEAD,          /* finished, waiting to be reaped  */
} task_state_t;

typedef void (*task_entry_t)(void *arg);

typedef struct task {
    /* The saved stack pointer must stay first: the scheduler treats a task's
       whole context as "the stack it was interrupted on". */
    uint32_t      esp;

    uint32_t      id;
    char          name[TASK_NAME_MAX];
    task_state_t  state;

    task_entry_t  entry;
    void         *arg;
    uint8_t      *stack;
    uint64_t      wake_at;      /* tick to become runnable again */
    uint64_t      cpu_ticks;    /* timer ticks spent running     */
    uint64_t      switches;
    struct task  *next;         /* circular run list             */
} task_t;

/* Turns the current execution context into the idle task and starts the
   scheduler. Called once, with interrupts already enabled. */
void    task_init(void);
bool    task_running(void);

task_t *task_spawn(const char *name, task_entry_t entry, void *arg);
task_t *task_current(void);
void    task_yield(void);
void    task_sleep_ms(uint32_t ms);
void    task_exit(void);
int     task_kill(uint32_t id);

int     task_count(void);
task_t *task_at(int index);
const char *task_state_name(task_state_t state);

/* Called from the interrupt path in idt.c, not by ordinary code. */
uint32_t task_schedule(uint32_t esp);   /* returns the stack to resume on */
void     task_account_tick(void);
void     task_reap(void);               /* idle task only */

#endif
