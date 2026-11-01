/* SPDX-License-Identifier: Apache-2.0 */
#include "platform/rtthread/hmi_watchdog_budget.h"
#include <aic_core.h>
#include <aic_hal.h>
#include <aic_drv_irq.h>
#include <rtthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* 看门狗复位前的现场记录。硬件复位只留下 Reset flag，看不出是谁占着 CPU；这里在 tick 中断里盯一个
 * 与监督线程同优先级的心跳线程，它停了 STALL_MS 就把正在跑的线程、被打断的 PC 和各中断源的次数增量
 * 打到串口。只有中断仍能进来（线程死循环、中断风暴）时才有输出；没有输出本身说明中断也停了。 */
#ifdef AIC_HMI_LOCKUP_SENTINEL

extern u32 g_irqcnt[MAX_IRQ_ENTRY];

#define HEARTBEAT_PERIOD_MS 100u
#define SNAPSHOT_MS 1000u
/* 必须小于 HMI_WATCHDOG_TIMEOUT_S：硬件复位之前还要留出几次报告。 */
#define STALL_MS 3000u
#define REPORT_INTERVAL_MS 1000u
#define MAX_REPORTS 6u
/* 启动阶段允许长时间占用 CPU（挂载、初始化），之前不判。 */
#define WARMUP_MS 20000u
#define HEARTBEAT_STACK 1024u
#define MS_TO_TICKS(ms) ((ms) * RT_TICK_PER_SECOND / 1000u)

static struct rt_thread heartbeat_thread;
static rt_ubase_t heartbeat_stack[HEARTBEAT_STACK / sizeof(rt_ubase_t)];
static u32 irq_snapshot[MAX_IRQ_ENTRY];
/* 线程写、tick 中断读；诊断用，不加锁。 */
static volatile rt_tick_t heartbeat_tick, snapshot_tick;
static volatile bool armed;
/* 只在 tick 中断里访问。 */
static rt_tick_t last_report;
static unsigned reports;

static void heartbeat_entry(void *parameter)
{
    (void)parameter;
    for (;;)
    {
        const rt_tick_t now = rt_tick_get();
        heartbeat_tick = now;
        if ((rt_tick_t)(now - snapshot_tick) >= MS_TO_TICKS(SNAPSHOT_MS))
        {
            memcpy(irq_snapshot, g_irqcnt, sizeof(irq_snapshot));
            snapshot_tick = now;
        }
        rt_thread_mdelay(HEARTBEAT_PERIOD_MS);
    }
}

/* 在 tick 中断里调用：这里与 SDK 的 trap_c 一样直接用 printf。 */
static void report_lockup(rt_tick_t age)
{
    rt_thread_t current = rt_thread_self();
    unsigned long mepc, mstatus;
    __asm__ volatile("csrr %0, mepc" : "=r"(mepc));
    __asm__ volatile("csrr %0, mstatus" : "=r"(mstatus));
    printf("lockup sentinel: heartbeat starved %lu ms, running=%s prio=%u mepc=%lx mstatus=%lx\n",
           (unsigned long)age * 1000ul / RT_TICK_PER_SECOND, current ? current->name : "?",
           current ? (unsigned)current->current_priority : 0u, mepc, mstatus);

    u32 best[2] = {0u, 0u};
    unsigned index[2] = {0u, 0u};
    for (unsigned i = 0u; i < MAX_IRQ_ENTRY; i++)
    {
        const u32 delta = g_irqcnt[i] - irq_snapshot[i];
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
    printf("  irq since snapshot (%lu ms ago): #%u +%lu, #%u +%lu (list_irq shows the numbers)\n",
           (unsigned long)(rt_tick_get() - snapshot_tick) * 1000ul / RT_TICK_PER_SECOND, index[0],
           (unsigned long)best[0], index[1], (unsigned long)best[1]);
}

static void tick_hook(void)
{
    if (!armed)
        return;
    const rt_tick_t now = rt_tick_get();
    if (now < MS_TO_TICKS(WARMUP_MS))
        return;
    const rt_tick_t age = now - heartbeat_tick;
    if (age < MS_TO_TICKS(STALL_MS))
    {
        if (reports)
        {
            printf("lockup sentinel: heartbeat resumed\n");
            reports = 0u;
        }
        return;
    }
    if (reports >= MAX_REPORTS || (reports && (rt_tick_t)(now - last_report) < MS_TO_TICKS(REPORT_INTERVAL_MS)))
        return;
    reports++;
    last_report = now;
    report_lockup(age);
}

static int hmi_lockup_sentinel_init(void)
{
    if (rt_thread_init(&heartbeat_thread, "hmi_hb", heartbeat_entry, RT_NULL, heartbeat_stack,
                       sizeof(heartbeat_stack), HMI_WATCHDOG_THREAD_PRIO, 10) != RT_EOK ||
        rt_thread_startup(&heartbeat_thread) != RT_EOK)
        return -RT_ERROR;
    heartbeat_tick = snapshot_tick = rt_tick_get();
    memcpy(irq_snapshot, g_irqcnt, sizeof(irq_snapshot));
    armed = true;
    rt_tick_sethook(tick_hook);
    return RT_EOK;
}
INIT_APP_EXPORT(hmi_lockup_sentinel_init);

#endif /* AIC_HMI_LOCKUP_SENTINEL */
