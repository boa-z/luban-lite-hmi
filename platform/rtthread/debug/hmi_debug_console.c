#include "platform/rtthread/debug/hmi_debug_console.h"
#include "platform/common/hmi_diag_commands.h"
#include "platform/rtthread/hmi_board_port.h"
#include <finsh.h>
#include <rtthread.h>
static struct rt_mutex owner_mutex, console_mutex;
static hmi_diagnostics_t *diagnostics;
static const hmi_build_info_t *identity;
/* 查询副本不占 MSH 小栈；console mutex 保护并发的命令调用。 */
static hmi_trace_entry_t entries[HMI_TRACE_CAPACITY];
static hmi_diag_snapshot_t snapshot;
static hmi_diag_signal_t signal;
bool hmi_debug_init(hmi_diagnostics_t *diag, const hmi_build_info_t *build)
{
    if (!diag || !build || diagnostics)
        return false;
    if (rt_mutex_init(&owner_mutex, "hmi.state", RT_IPC_FLAG_PRIO) != RT_EOK)
        return false;
    if (rt_mutex_init(&console_mutex, "hmi.query", RT_IPC_FLAG_PRIO) != RT_EOK)
    {
        rt_mutex_detach(&owner_mutex);
        return false;
    }
    identity = build;
    diagnostics = diag;
    return true;
}
void hmi_debug_lock(void)
{
    rt_mutex_take(&owner_mutex, RT_WAITING_FOREVER);
}
void hmi_debug_unlock(void)
{
    rt_mutex_release(&owner_mutex);
}
static void output(void *ctx, const char *line)
{
    (void)ctx;
    /* 这是 MSH 展示端；业务路径和锁内不调用 Console。 */
    rt_kprintf("%s\n", line);
}
static int hmi(int argc, char **argv)
{
    hmi_diag_query_t query;
    if (!hmi_diag_query(argc, (const char *const *)argv, &query))
    {
        output(NULL, "usage: hmi info|diag|runtime|can [0|1]|domain|signal <key>|touch|trace [dump|clear]");
        return -RT_EINVAL;
    }
    if (!diagnostics)
    {
        output(NULL, "hmi diagnostics not initialized");
        return -RT_ERROR;
    }
    rt_mutex_take(&console_mutex, RT_WAITING_FOREVER);
    hmi_diag_view_t view = {.snapshot = &snapshot, .build = identity};
    hmi_debug_lock();
    uint32_t now = (uint32_t)hmi_board_now_ms();
    hmi_diagnostics_snapshot(diagnostics, now, &snapshot);
    if (query.kind == HMI_QUERY_SIGNAL && hmi_diagnostics_signal(diagnostics, query.key, now, &signal))
        view.signal = &signal;
    if (query.kind == HMI_QUERY_TRACE_DUMP)
    {
        view.entries = entries;
        view.entry_count = hmi_trace_snapshot(&diagnostics->trace, entries, HMI_TRACE_CAPACITY);
    }
    if (query.kind == HMI_QUERY_TRACE_CLEAR)
    {
        view.cleared = diagnostics->trace.count;
        hmi_trace_clear(&diagnostics->trace);
    }
    hmi_debug_unlock();
    hmi_diag_render(&query, &view, output, NULL);
    rt_mutex_release(&console_mutex);
    return RT_EOK;
}
MSH_CMD_EXPORT(hmi, HMI diagnostics queries and trace history);
/* 后端只取得副本，不把诊断内部指针暴露给 ULog。 */
size_t hmi_debug_trace_since(uint32_t *cursor, hmi_trace_entry_t *out, size_t capacity)
{
    if (!diagnostics || !cursor || !out)
        return 0;
    hmi_debug_lock();
    uint32_t delta = diagnostics->trace.sequence - *cursor;
    if (delta > diagnostics->trace.count)
        delta = diagnostics->trace.count;
    if (delta > capacity)
        delta = (uint32_t)capacity;
    size_t count = hmi_trace_snapshot(&diagnostics->trace, out, delta);
    *cursor = diagnostics->trace.sequence;
    hmi_debug_unlock();
    return count;
}
