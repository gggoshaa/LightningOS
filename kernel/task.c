#include "task.h"
#include "isr.h"
#include "mem.h"
#include "timer.h"
#include "string.h"
#include "lock.h"
#include "panic.h"

/* Preemptive round robin scheduling of kernel threads.

   There is no paging and no user mode yet, so every task shares one address
   space and runs in ring 0. What makes a task a task is its stack: when the
   timer interrupt fires, the stub in interrupt.asm has already pushed the
   complete register set onto the stack of whatever was running, so switching
   context is only a matter of returning on a different stack pointer. */

/* The frame a stub leaves behind, and therefore the frame a brand new task
   has to fake so that its first `iret` lands in its entry point. A ring 0
   interrupt does not push ss:esp, so the frame ends at eflags. */
typedef struct {
    uint32_t ds;
    uint32_t edi, esi, ebp, esp_dummy, ebx, edx, ecx, eax;
    uint32_t int_no, err_code;
    uint32_t eip, cs, eflags;
} __attribute__((packed)) boot_frame_t;

#define KERNEL_CODE_SEG 0x08
#define KERNEL_DATA_SEG 0x10
#define EFLAGS_IF       0x0200
#define EFLAGS_RESERVED 0x0002

static task_t  tasks[TASK_MAX];
static task_t *current;
static task_t *idle;
static bool    started;
static uint32_t next_id;

static task_t *alloc_task(void)
{
    for (int i = 0; i < TASK_MAX; i++) {
        if (tasks[i].state == TASK_UNUSED)
            return &tasks[i];
    }
    return NULL;
}

static void list_insert(task_t *task)
{
    if (!current) {
        task->next = task;
        return;
    }
    task->next = current->next;
    current->next = task;
}

static void list_remove(task_t *task)
{
    task_t *prev = task;

    while (prev->next != task)
        prev = prev->next;
    prev->next = task->next;
    task->next = NULL;
}

/* Where a new task begins. Going through C rather than jumping straight at
   the entry point means a task that simply returns is still cleaned up. */
static void task_bootstrap(void)
{
    task_t *self = current;

    if (self && self->entry)
        self->entry(self->arg);
    task_exit();
}

task_t *task_spawn(const char *name, task_entry_t entry, void *arg)
{
    uint32_t flags = irq_save();
    task_t *task = alloc_task();

    if (!task) {
        irq_restore(flags);
        return NULL;
    }

    uint8_t *stack = (uint8_t *)kmalloc(TASK_STACK_SIZE);
    if (!stack) {
        irq_restore(flags);
        return NULL;
    }

    memset(task, 0, sizeof(*task));
    strncpy(task->name, name, TASK_NAME_MAX - 1);
    task->id = ++next_id;
    task->entry = entry;
    task->arg = arg;
    task->stack = stack;
    task->state = TASK_READY;

    /* Build the frame the stub epilogue will pop, at the top of the new
       stack. Sixteen bytes of headroom keep the frame clear of the end. */
    boot_frame_t *frame =
        (boot_frame_t *)(stack + TASK_STACK_SIZE - 16 - sizeof(boot_frame_t));
    memset(frame, 0, sizeof(*frame));

    frame->ds     = KERNEL_DATA_SEG;
    frame->cs     = KERNEL_CODE_SEG;
    frame->eip    = (uint32_t)task_bootstrap;
    frame->eflags = EFLAGS_IF | EFLAGS_RESERVED;

    task->esp = (uint32_t)frame;

    list_insert(task);
    irq_restore(flags);
    return task;
}

void task_init(void)
{
    uint32_t flags = irq_save();

    memset(tasks, 0, sizeof(tasks));

    /* The context that called us becomes the idle task. Its esp is filled in
       by the first switch away from it, so it needs no fake frame. */
    idle = &tasks[0];
    memset(idle, 0, sizeof(*idle));
    strncpy(idle->name, "idle", TASK_NAME_MAX - 1);
    idle->id = 0;
    idle->state = TASK_READY;
    idle->next = idle;

    current = idle;
    started = true;
    irq_restore(flags);
}

bool task_running(void) { return started; }
task_t *task_current(void) { return current; }

static void wake_sleepers(uint64_t now)
{
    task_t *task = current;

    do {
        if (task->state == TASK_SLEEPING && now >= task->wake_at)
            task->state = TASK_READY;
        task = task->next;
    } while (task != current);
}

/* Round robin: start after the task that just ran so nobody is starved. The
   idle task is only ever chosen when nothing else can run. */
static task_t *pick_next(void)
{
    task_t *task = current->next;
    task_t *start = task;

    do {
        if (task != idle && task->state == TASK_READY)
            return task;
        task = task->next;
    } while (task != start);

    return idle;
}

/* Called by the interrupt stubs with the interrupted stack pointer. Returns
   the stack pointer to resume on, which is a different one when we switch. */
uint32_t task_schedule(uint32_t esp)
{
    if (!started)
        return esp;

    current->esp = esp;

    wake_sleepers(timer_ticks());

    task_t *next = pick_next();
    if (next == current)
        return esp;

    next->switches++;
    current = next;
    return current->esp;
}

/* Charges the tick to whoever was running when it fired. */
void task_account_tick(void)
{
    if (started && current)
        current->cpu_ticks++;
}

void task_yield(void)
{
    if (!started)
        return;
    __asm__ volatile("int $0x30");
}

void task_sleep_ms(uint32_t ms)
{
    if (!started) {
        uint64_t target = timer_ticks() + ms / (1000 / TIMER_HZ);
        while (timer_ticks() < target)
            __asm__ volatile("hlt");
        return;
    }

    uint32_t flags = irq_save();
    current->wake_at = timer_ticks() + ms / (1000 / TIMER_HZ);
    current->state = TASK_SLEEPING;
    irq_restore(flags);

    task_yield();
}

void task_exit(void)
{
    uint32_t flags = irq_save();

    if (current == idle)
        panic("the idle task tried to exit");

    current->state = TASK_DEAD;
    irq_restore(flags);

    task_yield();

    /* A dead task is never picked again, so this is unreachable. */
    for (;;)
        __asm__ volatile("hlt");
}

int task_kill(uint32_t id)
{
    uint32_t flags = irq_save();
    int result = -1;

    task_t *task = current;
    do {
        if (task->id == id && task->state != TASK_UNUSED &&
            task->state != TASK_DEAD) {
            if (task == idle) {
                result = -2;            /* the idle task has to stay */
            } else {
                task->state = TASK_DEAD;
                result = 0;
            }
            break;
        }
        task = task->next;
    } while (task != current);

    irq_restore(flags);

    /* Killing yourself means never coming back. */
    if (result == 0 && task == current)
        task_yield();
    return result;
}

/* Frees the stacks of finished tasks. Only ever called from the idle task,
   which by definition is not running on any of them. */
void task_reap(void)
{
    uint32_t flags = irq_save();

    task_t *task = current->next;
    while (task != current) {
        task_t *next = task->next;

        if (task->state == TASK_DEAD) {
            list_remove(task);
            if (task->stack)
                kfree(task->stack);
            task->state = TASK_UNUSED;
            task->stack = NULL;
        }
        task = next;
    }
    irq_restore(flags);
}

int task_count(void)
{
    uint32_t flags = irq_save();
    int count = 0;

    for (int i = 0; i < TASK_MAX; i++) {
        if (tasks[i].state != TASK_UNUSED)
            count++;
    }
    irq_restore(flags);
    return count;
}

task_t *task_at(int index)
{
    int seen = 0;

    for (int i = 0; i < TASK_MAX; i++) {
        if (tasks[i].state == TASK_UNUSED)
            continue;
        if (seen == index)
            return &tasks[i];
        seen++;
    }
    return NULL;
}

const char *task_state_name(task_state_t state)
{
    switch (state) {
    case TASK_READY:    return "ready";
    case TASK_SLEEPING: return "sleeping";
    case TASK_DEAD:     return "dead";
    default:            return "unused";
    }
}
