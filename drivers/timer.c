#include "timer.h"
#include "isr.h"
#include "io.h"
#include "task.h"

#define PIT_CHANNEL0 0x40
#define PIT_COMMAND  0x43
#define PIT_BASE_HZ  1193182

static volatile uint64_t ticks;

static void timer_callback(registers_t *regs)
{
    (void)regs;
    ticks++;
    task_account_tick();
}

void timer_init(void)
{
    uint32_t divisor = PIT_BASE_HZ / TIMER_HZ;

    outb(PIT_COMMAND, 0x36);                        /* channel 0, lo/hi, mode 3 */
    outb(PIT_CHANNEL0, (uint8_t)(divisor & 0xFF));
    outb(PIT_CHANNEL0, (uint8_t)((divisor >> 8) & 0xFF));

    irq_install_handler(0, timer_callback);
}

uint64_t timer_ticks(void)
{
    return ticks;
}

uint64_t timer_uptime_ms(void)
{
    return ticks * (1000 / TIMER_HZ);
}

/* Once the scheduler is up this hands the CPU to somebody else instead of
   spinning on hlt. */
void sleep_ms(uint32_t ms)
{
    if (task_running()) {
        task_sleep_ms(ms);
        return;
    }

    uint64_t target = ticks + (ms / (1000 / TIMER_HZ));
    while (ticks < target)
        __asm__ volatile("hlt");
}
