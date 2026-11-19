#define LOG_TAG "hmi.wdog"
#define LOG_LVL LOG_LVL_INFO
#include "platform/rtthread/hmi_watchdog_port.h"
#include "platform/rtthread/hmi_execution_port.h"
#include "platform/rtthread/hmi_watchdog_budget.h"
#include "runtime/hmi_watchdog.h"
#include <rtdevice.h>
#include <rtthread.h>
#include <ulog.h>

/* 启用开关来自 Kconfig；关掉时端口不编译，只留一个记录用的空实现。 */
#ifdef AIC_HMI_WATCHDOG

/* 监督线程独占状态；单核板端用调度临界区保护状态推进与诊断快照，区内不做设备 I/O。 */
static rt_device_t wdt_device;
static struct rt_thread watchdog_thread;
static rt_ubase_t watchdog_stack[HMI_WATCHDOG_THREAD_STACK / sizeof(rt_ubase_t)];
static hmi_watchdog_state_t state;
static hmi_watchdog_policy_t policy;
static bool device_present, armed;

static const char *const channel_names[HMI_WATCHDOG_CHANNELS] = {"app", "ui"};

static bool arm_watchdog(void)
{
    uint16_t timeout = (uint16_t)HMI_WATCHDOG_TIMEOUT_S;
    if (rt_device_control(wdt_device, RT_DEVICE_CTRL_WDT_SET_TIMEOUT, &timeout) != RT_EOK)
    {
        LOG_E("cannot set WDT timeout to %us", (unsigned)HMI_WATCHDOG_TIMEOUT_S);
        return false;
    }
    if (rt_device_control(wdt_device, RT_DEVICE_CTRL_WDT_START, RT_NULL) != RT_EOK)
    {
        LOG_E("cannot start WDT");
        return false;
    }
    LOG_I("WDT armed: timeout=%us poll=%ums stall app=%ums ui=%ums", (unsigned)HMI_WATCHDOG_TIMEOUT_S,
          (unsigned)HMI_WATCHDOG_POLL_MS, (unsigned)HMI_WATCHDOG_APP_STALL_MS,
          (unsigned)HMI_WATCHDOG_UI_STALL_MS);
    return true;
}

static void watchdog_entry(void *parameter)
{
    (void)parameter;
    uint32_t last_report = 0u;
    for (;;)
    {
        hmi_execution_liveness_t live;
        /* runtime 未启动时不进入监督态：启动阶段与启动失败都不得复位。 */
        if (hmi_execution_liveness(&live))
        {
            if (!armed)
                armed = arm_watchdog();
            if (armed)
            {
                hmi_watchdog_input_t input = {0};
                input.counter[HMI_WATCHDOG_APP] = live.app_runs;
                input.counter[HMI_WATCHDOG_UI] = live.ui_ticks;
                input.expected = 1u << HMI_WATCHDOG_APP;
                if (live.expect_ui)
                    input.expected |= 1u << HMI_WATCHDOG_UI;
                const uint32_t now = (uint32_t)rt_tick_get_millisecond();
                rt_enter_critical();
                const bool feed = hmi_watchdog_step(&state, &policy, &input, now);
                rt_exit_critical();
                if (feed)
                    (void)rt_device_control(wdt_device, RT_DEVICE_CTRL_WDT_KEEPALIVE, RT_NULL);
                else if (now - last_report > 1000u)
                {
                    /* 状态变化后仍停滞，最多每秒上报一次，避免监督线程自己刷屏。 */
                    last_report = now;
                    const bool known = state.stalled < HMI_WATCHDOG_CHANNELS;
                    LOG_E("feed withheld: %s stalled over %ums; hardware reset follows",
                          known ? channel_names[state.stalled] : "unknown",
                          known ? (unsigned)policy.stall_ms[state.stalled] : 0u);
                }
            }
        }
        rt_thread_mdelay(HMI_WATCHDOG_POLL_MS);
    }
}

int hmi_watchdog_port_init(void)
{
    wdt_device = rt_device_find("wdt");
    if (!wdt_device)
    {
        /* 没有看门狗设备的板子保持关闭，不影响其余功能。 */
        LOG_W("no wdt device; watchdog supervision stays off");
        return RT_EOK;
    }
    if (rt_device_init(wdt_device) != RT_EOK)
    {
        LOG_E("cannot initialize wdt device; supervision stays off");
        wdt_device = RT_NULL;
        return RT_EOK;
    }
    policy.enabled = true;
    policy.stall_ms[HMI_WATCHDOG_APP] = HMI_WATCHDOG_APP_STALL_MS;
    policy.stall_ms[HMI_WATCHDOG_UI] = HMI_WATCHDOG_UI_STALL_MS;
    hmi_watchdog_init(&state);
    device_present = true;
    if (rt_thread_init(&watchdog_thread, "hmi_wdog", watchdog_entry, RT_NULL, watchdog_stack,
                       sizeof(watchdog_stack), HMI_WATCHDOG_THREAD_PRIO, 10) != RT_EOK ||
        rt_thread_startup(&watchdog_thread) != RT_EOK)
        LOG_E("cannot start watchdog thread; supervision stays off");
    return RT_EOK;
}

bool hmi_watchdog_report(hmi_watchdog_report_t *out)
{
    if (!out || !device_present)
        return false;
    rt_enter_critical();
    *out = (hmi_watchdog_report_t){.device_present = true,
                                   .armed = armed,
                                   .supervising = state.supervising,
                                   .feeds = state.feeds,
                                   .withheld = state.withheld,
                                   .stalls = state.stalls,
                                   .transitions = state.transitions,
                                   .stalled_channel = (uint32_t)state.stalled,
                                   .timeout_s = (uint16_t)HMI_WATCHDOG_TIMEOUT_S};
    rt_exit_critical();
    return true;
}

static int hmi_watchdog_msh(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    hmi_watchdog_report_t report;
    if (!hmi_watchdog_report(&report))
    {
        rt_kprintf("watchdog unavailable (no wdt device or disabled)\n");
        return -RT_ERROR;
    }
    rt_kprintf("watchdog armed=%u supervising=%u timeout_s=%u\n", report.armed ? 1u : 0u,
               report.supervising ? 1u : 0u, (unsigned)report.timeout_s);
    rt_kprintf("watchdog feeds=%u withheld=%u stalls=%u transitions=%u stalled=%u\n", report.feeds,
               report.withheld, report.stalls, report.transitions, report.stalled_channel);
    for (unsigned i = 0u; i < HMI_WATCHDOG_CHANNELS; ++i)
        rt_kprintf("watchdog channel=%s stall_ms=%u\n", channel_names[i], (unsigned)policy.stall_ms[i]);
    return RT_EOK;
}
MSH_CMD_EXPORT_ALIAS(hmi_watchdog_msh, hmi_watchdog, Show watchdog supervision state);

#else

int hmi_watchdog_port_init(void)
{
    LOG_W("watchdog supervision is disabled by AIC_HMI_WATCHDOG");
    return RT_EOK;
}

bool hmi_watchdog_report(hmi_watchdog_report_t *out)
{
    (void)out;
    return false;
}

#endif
INIT_APP_EXPORT(hmi_watchdog_port_init);
