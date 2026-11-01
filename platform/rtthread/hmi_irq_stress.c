/* SPDX-License-Identifier: Apache-2.0 */
#include "platform/rtthread/hmi_watchdog_budget.h"
#include <aic_core.h>
#include <aic_hal.h>
#include <aic_drv_irq.h>
#include <finsh.h>
#include <rthw.h>
#include <rtthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys_freq.h>

#ifdef AIC_HMI_IRQ_STRESS

/* 对比 SDK 自带的 rt_hw_interrupt_disable（跨 cache line）与 hmi_irq_guard.S 里对齐的那份。
 * 两份都只做 csrrci a0, mstatus, 8；这里预置 a0 再调用，写回丢失会被直接看到，不用等它写坏 mstatus。 */
extern rt_base_t __real_rt_hw_interrupt_disable(void);
extern rt_base_t __wrap_rt_hw_interrupt_disable(void);
/* SDK trap_c 打印完寄存器后调用；本应用是唯一设置者，只在压力运行期间挂接。 */
extern void (*trap_c_callback)(void);
/* SDK 的逐中断号计数（含 GPIO 引脚中断），list_irq 打印的就是它。 */
extern u32 g_irqcnt[MAX_IRQ_ENTRY];

#define MSTATUS_MIE 0x8u
/* 不是合法的 mstatus：MIE 位为 0。 */
#define LEVEL_MARKER 0xa5a5a5a5c3c3c3c0ul
#define BURST 256u
/* 一个 burst 256 次调用约 16 us，连续 50000 个 burst（约 0.8 s）tick 都没推进就按停摆处理。 */
#define STALL_BURSTS 50000u
#define DEFAULT_SECONDS 60u
#define MAX_SECONDS 86400u
#define REPORT_SECONDS 2u
/* 压测期间的优先级必须低于看门狗监督线程：跑飞的压测不能饿死喂狗，
 * 否则“压测自己饿死监督线程”和“CPU 真的卡死”会表现成同一次复位。 */
#define STRESS_PRIORITY (HMI_WATCHDOG_THREAD_PRIO + 1)

typedef rt_base_t (*disable_fn_t)(void);

/* 每个 burst 结束时刷新；异常报告读到的是最近一次完整 burst。 */
static volatile struct
{
    const char *mode;
    rt_tick_t start;
    unsigned long iterations;
    unsigned long unwritten;     /* 返回值仍是预置的标记：csrrci 没写 a0 */
    unsigned long no_mie;        /* 返回的 level 没有 MIE 位 */
    unsigned long lost_mie;      /* enable 之后 MIE 仍为 0 */
    unsigned long tick_stalls;   /* 约 0.8 s 内 tick 没有推进（中断被关死或定时器中断丢失） */
    unsigned long stall_mstatus; /* 最近一次停摆时的 mstatus / mie / mip */
    unsigned long stall_mie;
    unsigned long stall_mip;
} stats;

/* 一个报告周期的窗口。mcycle 与 4 MHz 计时器只在 burst 期间累加，得到的主频不受抢占和空闲影响：
 * 主频减半说明时钟变了；主频不变而每毫秒迭代数减半，说明 CPU 被中断或其他线程占走。 */
static struct
{
    uint64_t cycles;
    uint64_t timer;
    unsigned long iterations;
    u32 irq[MAX_IRQ_ENTRY];
} window;

static inline rt_base_t call_disable(disable_fn_t fn)
{
    register rt_base_t level __asm__("a0") = (rt_base_t)LEVEL_MARKER;
    register disable_fn_t target __asm__("t0") = fn;
    __asm__ volatile("jalr ra, 0(%1)" : "+r"(level) : "r"(target) : "ra", "memory");
    return level;
}

static inline unsigned long read_mstatus(void)
{
    unsigned long value;
    __asm__ volatile("csrr %0, mstatus" : "=r"(value));
    return value;
}

static inline uint64_t read_mcycle(void)
{
    uint64_t value;
    __asm__ volatile("csrr %0, mcycle" : "=r"(value));
    return value;
}

static void note_tick_stall(void)
{
    unsigned long mie, mip;
    __asm__ volatile("csrr %0, mie" : "=r"(mie));
    __asm__ volatile("csrr %0, mip" : "=r"(mip));
    stats.stall_mstatus = read_mstatus();
    stats.stall_mie = mie;
    stats.stall_mip = mip;
    stats.tick_stalls++;
    /* 先把现场记下来，再尝试恢复中断，让压测和系统继续走。 */
    __asm__ volatile("csrsi mstatus, 8");
}

static void run_burst(disable_fn_t fn)
{
    unsigned long unwritten = 0u, no_mie = 0u, lost_mie = 0u;
    for (unsigned i = 0u; i < BURST; i++)
    {
        const rt_base_t level = call_disable(fn);
        if (level == (rt_base_t)LEVEL_MARKER || !(level & MSTATUS_MIE))
        {
            if (level == (rt_base_t)LEVEL_MARKER)
                unwritten++;
            else
                no_mie++;
            /* level 不可信，不能写回 mstatus；本线程调用前中断是开着的，直接恢复。 */
            __asm__ volatile("csrsi mstatus, 8");
            continue;
        }
        rt_hw_interrupt_enable(level);
        if (!(read_mstatus() & MSTATUS_MIE))
        {
            lost_mie++;
            __asm__ volatile("csrsi mstatus, 8");
        }
    }
    stats.iterations += BURST;
    stats.unwritten += unwritten;
    stats.no_mie += no_mie;
    stats.lost_mie += lost_mie;
}

static void report_in_trap(void)
{
    printf("irq_stress at trap: mode=%s iterations=%lu elapsed_ms=%lu unwritten=%lu no_mie=%lu lost_mie=%lu "
           "tick_stalls=%lu\n",
           stats.mode, stats.iterations, (unsigned long)(rt_tick_get() - stats.start) * 1000ul / RT_TICK_PER_SECOND,
           stats.unwritten, stats.no_mie, stats.lost_mie, stats.tick_stalls);
}

static void report(const char *prefix)
{
    const unsigned long timer_mhz = (unsigned long)drv_get_sys_freq() / 1000000ul;
    const unsigned long burst_us = window.timer ? (unsigned long)(window.timer / timer_mhz) : 0ul;
    const unsigned long mhz = window.timer ? (unsigned long)(window.cycles * timer_mhz / window.timer) : 0ul;
    const unsigned long per_ms = burst_us ? (stats.iterations - window.iterations) * 1000ul / burst_us : 0ul;

    /* 这个周期里次数增加最多的两个中断源（SDK 的中断号，GPIO 引脚排在 MAX_IRQn 之后）。 */
    u32 best[2] = {0u, 0u};
    unsigned index[2] = {0u, 0u};
    for (unsigned i = 0u; i < MAX_IRQ_ENTRY; i++)
    {
        const u32 delta = g_irqcnt[i] - window.irq[i];
        if (delta > best[0])
        {
            best[1] = best[0];
            index[1] = index[0];
            best[0] = delta;
            index[0] = i;
        }
        else if (delta > best[1])
        {
            best[1] = delta;
            index[1] = i;
        }
    }
    rt_kprintf("%s mode=%s iterations=%lu elapsed=%lus unwritten=%lu no_mie=%lu lost_mie=%lu tick_stalls=%lu "
               "mhz=%lu per_ms=%lu irq=#%u+%lu,#%u+%lu\n",
               prefix, stats.mode, stats.iterations, (unsigned long)(rt_tick_get() - stats.start) / RT_TICK_PER_SECOND,
               stats.unwritten, stats.no_mie, stats.lost_mie, stats.tick_stalls, mhz, per_ms, index[0],
               (unsigned long)best[0], index[1], (unsigned long)best[1]);
    if (stats.tick_stalls)
        rt_kprintf("  last stall: mstatus=%lx mie=%lx mip=%lx\n", stats.stall_mstatus, stats.stall_mie,
                   stats.stall_mip);

    window.cycles = window.timer = 0u;
    window.iterations = stats.iterations;
    memcpy(window.irq, g_irqcnt, sizeof(window.irq));
}

static int hmi_irq_stress_msh(int argc, char **argv)
{
    disable_fn_t fn = RT_NULL;
    if (argc >= 2 && !strcmp(argv[1], "real"))
        fn = __real_rt_hw_interrupt_disable;
    else if (argc >= 2 && !strcmp(argv[1], "aligned"))
        fn = __wrap_rt_hw_interrupt_disable;
    const long seconds = argc >= 3 ? atol(argv[2]) : (long)DEFAULT_SECONDS;
    if (!fn || argc > 3 || seconds < 1 || seconds > (long)MAX_SECONDS)
    {
        rt_kprintf("usage: hmi_irq_stress <real|aligned> [seconds]\n"
                   "  real    SDK rt_hw_interrupt_disable at %p (line offset %u)\n"
                   "  aligned hmi_irq_guard.S copy at %p (line offset %u)\n",
                   (void *)__real_rt_hw_interrupt_disable, (unsigned)((uintptr_t)__real_rt_hw_interrupt_disable & 63u),
                   (void *)__wrap_rt_hw_interrupt_disable, (unsigned)((uintptr_t)__wrap_rt_hw_interrupt_disable & 63u));
        return -RT_ERROR;
    }

    /* 命令就在 shell 线程里跑：运行期间降到监督线程之下，结束后恢复。 */
    rt_thread_t self = rt_thread_self();
    const rt_uint8_t original_priority = self->current_priority;
    rt_uint8_t stress_priority = (rt_uint8_t)STRESS_PRIORITY;
    (void)rt_thread_control(self, RT_THREAD_CTRL_CHANGE_PRIORITY, &stress_priority);

    stats.mode = argv[1];
    stats.iterations = stats.unwritten = stats.no_mie = stats.lost_mie = 0u;
    stats.tick_stalls = stats.stall_mstatus = stats.stall_mie = stats.stall_mip = 0u;
    stats.start = rt_tick_get();
    window.cycles = window.timer = 0u;
    window.iterations = 0u;
    memcpy(window.irq, g_irqcnt, sizeof(window.irq));
    trap_c_callback = report_in_trap;
    rt_kprintf("irq_stress start: mode=%s %lds priority=%u, function at %p (line offset %u)\n", stats.mode, seconds,
               (unsigned)stress_priority, (void *)fn, (unsigned)((uintptr_t)fn & 63u));
    const rt_tick_t duration = (rt_tick_t)seconds * RT_TICK_PER_SECOND;
    rt_tick_t next_report = REPORT_SECONDS * RT_TICK_PER_SECOND;
    while ((rt_tick_t)(rt_tick_get() - stats.start) < duration)
    {
        /* 每个 tick 内连续调用，tick 与其他中断才有机会落在这条指令上；
         * 随后让出 1 ms，空闲线程和低优先级线程照常运行。 */
        const uint64_t cycles0 = read_mcycle();
        const uint64_t timer0 = aic_get_ticks();
        const rt_tick_t slice = rt_tick_get();
        unsigned bursts = 0u;
        while (rt_tick_get() == slice)
        {
            run_burst(fn);
            if (++bursts > STALL_BURSTS)
            {
                note_tick_stall();
                break;
            }
        }
        window.cycles += read_mcycle() - cycles0;
        window.timer += aic_get_ticks() - timer0;
        if ((rt_tick_t)(rt_tick_get() - stats.start) >= next_report)
        {
            report("irq_stress");
            next_report += REPORT_SECONDS * RT_TICK_PER_SECOND;
        }
        rt_thread_mdelay(1);
    }
    trap_c_callback = RT_NULL;
    rt_uint8_t restore_priority = original_priority;
    (void)rt_thread_control(self, RT_THREAD_CTRL_CHANGE_PRIORITY, &restore_priority);
    report("irq_stress done:");
    return stats.unwritten || stats.no_mie || stats.lost_mie || stats.tick_stalls ? -RT_ERROR : RT_EOK;
}
MSH_CMD_EXPORT_ALIAS(hmi_irq_stress_msh, hmi_irq_stress, Compare the SDK and aligned rt_hw_interrupt_disable);

#endif /* AIC_HMI_IRQ_STRESS */
