/* SPDX-License-Identifier: Apache-2.0 */
#define LOG_TAG "hmi.exec"
#define LOG_LVL LOG_LVL_INFO
#include "platform/rtthread/hmi_execution_port.h"
#include "contracts/hmi_time.h"
#include "core/hmi_snapshot.h"
#include "platform/rtthread/debug/hmi_debug_console.h"
#include "platform/rtthread/hmi_blackbox.h"
#include "platform/rtthread/hmi_board_port.h"
#include "platform/rtthread/hmi_board_settings.h"
#include "runtime/hmi_periodic.h"
#include "runtime/hmi_runtime.h"
#include <finsh.h>
#include <rtthread.h>
#include <string.h>
#include <ulog.h>

/* 容量是板端静态预算，不限制 Product 目录大小；周期条目超过预算时启动失败。 */
#ifndef HMI_BOARD_PERIODIC_SLOTS
#define HMI_BOARD_PERIODIC_SLOTS 8u
#endif
#define TX_DEPTH 16u
#define ACTION_DEPTH 8u
#define RX_BUDGET 64u
#define RX_SLICE_MS 2u
#define TX_SLICE_MS 5u
#define APP_ACTIVE_BUDGET_MS 5u
#define APP_POLL_MS 5u
#define CAN_DIAG_POLL_MS 100u
#define NO_PERIODIC 0xFFu
#define POOL_WORDS(type, count)                                                                              \
    (((RT_ALIGN(sizeof(type), RT_ALIGN_SIZE) + sizeof(void *)) / sizeof(rt_ubase_t)) * (count))

typedef struct
{
    hmi_can_frame_t frame;
    hmi_can_tx_options_t options;
    uint32_t generation, expires_ms;
    uint8_t periodic; /* 周期条目序号；NO_PERIODIC 表示普通帧 */
    bool expires;     /* expires_ms 之前未交给驱动则作废 */
} tx_message_t;
typedef struct
{
    struct rt_messagequeue ordinary, urgent;
    struct rt_semaphore wake;
    rt_ubase_t ordinary_pool[POOL_WORDS(tx_message_t, TX_DEPTH)],
        urgent_pool[POOL_WORDS(tx_message_t, TX_DEPTH)];
    struct rt_thread thread;
    rt_ubase_t stack[2048u / sizeof(rt_ubase_t)];
    bool open;
    volatile bool stop_requested;
    bool stopped;
    uint32_t error_log_ms, error_logged;
    /* 普通帧失败退避：窗口内不再调用驱动，避免总线故障时高优先级 TX 线程反复走 HAL
       拒绝路径并同步打印日志，把 LVGL 线程挤掉。优先帧不受此限。 */
    uint32_t fail_until_ms, fail_window_ms;
    uint32_t requested_bitrate;
    volatile unsigned configure_state; /* 0 空闲，1 待执行，2 成功，3 失败 */
    hmi_can_rx_ids_t rx_ids;
    bool rx_ids_pending;
    /* 以下计数只由本总线的发送线程写。 */
    volatile uint32_t ok, error, dropped, last_ms;
    volatile uint32_t attempts, completions, call_max_ms;
} tx_worker_t;

static hmi_execution_config_t config;
static hmi_runtime_t runtime;
static hmi_diagnostics_t app_diag;
static uint32_t trace_cursor;
static tx_worker_t tx[HMI_BUS_COUNT];
static hmi_deadline_t periodic_deadline[HMI_BOARD_PERIODIC_SLOTS];
static volatile bool periodic_in_flight[HMI_BOARD_PERIODIC_SLOTS];
static struct rt_semaphore wake;
static struct rt_messagequeue actions;
static rt_ubase_t action_pool[POOL_WORDS(hmi_action_t, ACTION_DEPTH)];
static struct rt_thread app_thread;
static rt_ubase_t app_stack[12288u / sizeof(rt_ubase_t)];
/* 发布区：hmi_debug_lock 同时保护公共诊断与给 UI 的快照，查询端读同一份数据。 */
static hmi_snapshot_t published;
static bool view_ready;
/* 运行时线程写、其他线程只读的字宽状态。 */
static volatile bool initialized, started, ui_presented;
static volatile uint32_t generation, app_runs, ui_ticks;
static volatile uint32_t rx_budget_hits, rx_max_ms;
static uint32_t can_diag_at;
static uint32_t actions_rejected, tx_full;

static bool tx_enqueue(const hmi_can_frame_t *frame, uint32_t expires_ms, bool expires, uint8_t periodic,
                       const hmi_can_tx_options_t *options)
{
    if (!frame || (unsigned)frame->bus >= HMI_BUS_COUNT || frame->size > 8u || !tx[frame->bus].open ||
        tx[frame->bus].stop_requested)
        return false;
    if (frame->id > (frame->extended ? 0x1FFFFFFFu : 0x7FFu))
        return false;
    tx_worker_t *t = &tx[frame->bus];
    tx_message_t m = {.frame = *frame,
                      .generation = generation,
                      .expires_ms = expires_ms,
                      .periodic = periodic,
                      .expires = expires};
    if (options)
        m.options = *options;
    if (rt_mq_send(m.options.priority ? &t->urgent : &t->ordinary, &m, sizeof(m)) != RT_EOK)
    {
        hmi_diag_increment(&tx_full);
        return false;
    }
    (void)rt_sem_release(&t->wake);
    return true;
}
static bool runtime_send(void *context, const hmi_can_frame_t *frame)
{
    (void)context;
    return tx_enqueue(frame, 0u, false, NO_PERIODIC, NULL);
}
static bool runtime_stop(void *context, hmi_bus_role_t bus)
{
    (void)context;
    if ((unsigned)bus >= HMI_BUS_COUNT || !tx[bus].open)
        return false;
    tx[bus].stop_requested = true;
    (void)rt_sem_release(&tx[bus].wake);
    return true;
}
static bool runtime_send_checked(void *context, const hmi_can_frame_t *frame,
                                 const hmi_can_tx_options_t *options)
{
    (void)context;
    return options && tx_enqueue(frame, options->expires_ms, options->expires, NO_PERIODIC, options);
}
static hmi_can_config_result_t runtime_configure(void *context, hmi_bus_role_t bus, uint32_t bitrate)
{
    (void)context;
    if ((unsigned)bus >= HMI_BUS_COUNT || !tx[bus].open || tx[bus].stop_requested || !bitrate)
        return HMI_CAN_CONFIG_FAILED;
    tx_worker_t *t = &tx[bus];
    unsigned state = __atomic_load_n(&t->configure_state, __ATOMIC_ACQUIRE);
    /* 上一调用方取消后可用新速率替换已完成但未取走的结果。 */
    if (!state || (state > 1u && t->requested_bitrate != bitrate))
    {
        t->requested_bitrate = bitrate;
        __atomic_store_n(&t->configure_state, 1u, __ATOMIC_RELEASE);
        (void)rt_sem_release(&t->wake);
        return HMI_CAN_CONFIG_PENDING;
    }
    if (t->requested_bitrate != bitrate)
        return HMI_CAN_CONFIG_FAILED;
    if (state == 1u)
        return HMI_CAN_CONFIG_PENDING;
    __atomic_store_n(&t->configure_state, 0u, __ATOMIC_RELEASE);
    return state == 2u ? HMI_CAN_CONFIG_OK : HMI_CAN_CONFIG_FAILED;
}
static bool runtime_set_rx_ids(void *context, hmi_bus_role_t bus, const hmi_can_rx_ids_t *ids)
{
    (void)context;
    if ((unsigned)bus >= HMI_BUS_COUNT || !ids || !ids->count || ids->count > HMI_CAN_RX_ID_LIMIT)
        return false;
    tx_worker_t *t = &tx[bus];
    if (!t->open || t->stop_requested || __atomic_load_n(&t->rx_ids_pending, __ATOMIC_ACQUIRE))
        return false;
    t->rx_ids = *ids;
    __atomic_store_n(&t->rx_ids_pending, true, __ATOMIC_RELEASE);
    (void)rt_sem_release(&t->wake);
    return true;
}
static const hmi_can_tx_port_t runtime_tx = {.send = runtime_send,
                                             .stop = runtime_stop,
                                             .send_checked = runtime_send_checked,
                                             .configure = runtime_configure,
                                             .set_rx_ids = runtime_set_rx_ids};

/* 发送线程的一次服务：取一帧并交给驱动；队列为空返回 false。 */
static bool tx_service(tx_worker_t *t)
{
    if (t->stop_requested && !t->stopped)
    {
        hmi_bb_phase(HMI_BB_TX0 + (t - tx), HMI_BB_CAN_CONFIGURE, 0);
        t->stopped = hmi_board_can_stop((hmi_bus_role_t)(t - tx));
        hmi_bb_complete(HMI_BB_TX0 + (t - tx), !t->stopped, 0);
        if (!t->stopped)
            rt_thread_mdelay(100);
        return true;
    }
    if (!t->stop_requested && __atomic_load_n(&t->configure_state, __ATOMIC_ACQUIRE) == 1u)
    {
        hmi_bb_phase(HMI_BB_TX0 + (t - tx), HMI_BB_CAN_CONFIGURE, t->requested_bitrate);
        bool ok = hmi_board_can_configure((hmi_bus_role_t)(t - tx), t->requested_bitrate);
        hmi_bb_complete(HMI_BB_TX0 + (t - tx), !ok, 0);
        __atomic_store_n(&t->configure_state, ok ? 2u : 3u, __ATOMIC_RELEASE);
        return true;
    }
    if (!t->stop_requested && __atomic_load_n(&t->rx_ids_pending, __ATOMIC_ACQUIRE))
    {
        hmi_can_config_result_t result = hmi_board_can_set_rx_ids((hmi_bus_role_t)(t - tx), &t->rx_ids);
        if (result != HMI_CAN_CONFIG_PENDING)
        {
            __atomic_store_n(&t->rx_ids_pending, false, __ATOMIC_RELEASE);
            return true;
        }
    }
    tx_message_t m;
    if (rt_mq_recv(&t->urgent, &m, sizeof(m), 0) != RT_EOK &&
        rt_mq_recv(&t->ordinary, &m, sizeof(m), 0) != RT_EOK)
        return false;
    const bool explicit_priority = m.options.priority;
    uint32_t now = hmi_board_now_ms();
    bool stale =
        t->stop_requested || m.generation != generation || (m.expires && hmi_time_reached(now, m.expires_ms));
    if (!stale && m.options.valid)
        stale = !m.options.valid(m.options.context, m.options.tag);
    /* 检查期间可能发生下线或会话切换，交给驱动前再次读取。 */
    stale = stale || t->stop_requested || m.generation != generation ||
            (m.expires && hmi_time_reached(hmi_board_now_ms(), m.expires_ms));
    /* 普通帧退避窗口内直接丢弃：总线故障时 TX 线程（优先级 18）原本会在 HAL 拒绝后立刻
       服务下一项，同步串口日志连续占住 CPU，LVGL（优先级 20）整屏重绘就被拖成卡顿。
       优先帧仍立即尝试，避免退避推迟协议调度。失败仍不重放。 */
    bool blocked = !explicit_priority && t->fail_until_ms && !hmi_time_reached(now, t->fail_until_ms);
    bool ok = false;
    if (!stale && !blocked)
    {
        const uint32_t begin = hmi_board_now_ms();
        ++t->attempts;
        hmi_bb_phase(HMI_BB_TX0 + (t - tx), HMI_BB_CAN_WRITE, m.frame.id);
        ok = hmi_board_can_send(&m.frame);
        hmi_bb_complete(HMI_BB_TX0 + (t - tx), !ok, 0);
        const uint32_t elapsed = hmi_board_now_ms() - begin;
        if (elapsed > t->call_max_ms)
            t->call_max_ms = elapsed;
        ++t->completions;
    }
    if (m.periodic != NO_PERIODIC)
        periodic_in_flight[m.periodic] = false;
    if (stale || blocked)
        ++t->dropped;
    else if (ok)
    {
        ++t->ok;
        t->last_ms = hmi_board_now_ms();
        t->fail_until_ms = 0u;
        t->fail_window_ms = 0u;
    }
    else
    {
        /* 失败不重放、不阻塞后续优先帧；日志最多每秒汇总一次。 */
        ++t->error;
        now = hmi_board_now_ms();
        const uint32_t window = t->fail_window_ms ? t->fail_window_ms * 2u : 50u;
        t->fail_window_ms = window > 200u ? 200u : window;
        t->fail_until_ms = now + t->fail_window_ms;
        if (!t->error_logged || (uint32_t)(now - t->error_log_ms) >= 1000u)
        {
            LOG_W("tx bus=%u failed=%u (+%u)", (unsigned)(t - tx), (unsigned)t->error,
                  (unsigned)(t->error - t->error_logged));
            t->error_logged = t->error;
            t->error_log_ms = now;
        }
    }
    return true;
}
static void tx_entry(void *arg)
{
    tx_worker_t *t = arg;
    uint32_t active_since = hmi_board_now_ms();
    for (;;)
    {
        (void)rt_sem_control(&t->wake, RT_IPC_CMD_RESET, RT_NULL);
        if (!tx_service(t))
        {
            const bool filter_pending = !t->stop_requested &&
                                        __atomic_load_n(&t->rx_ids_pending, __ATOMIC_ACQUIRE);
            (void)rt_sem_take(&t->wake, filter_pending ? rt_tick_from_millisecond(10u) : RT_WAITING_FOREVER);
            active_since = hmi_board_now_ms();
        }
        else if ((uint32_t)(hmi_board_now_ms() - active_since) >= TX_SLICE_MS)
        {
            rt_thread_mdelay(1);
            active_since = hmi_board_now_ms();
        }
    }
}

/* 会话代次切换（启动、进入/退出升级维护）：复位适配器与 Product 工作流，旧代次未发出的帧作废。 */
static void restart_session(void)
{
    hmi_runtime_connection(&runtime, false);
    hmi_runtime_connection(&runtime, true);
    generation = runtime.generation;
    memset(periodic_deadline, 0, sizeof(periodic_deadline));
    if (config.product->app_reset)
        config.product->app_reset(runtime.generation);
    hmi_core_connection(config.core, false, runtime.generation);
    hmi_core_connection(config.core, true, runtime.generation);
}
static void apply_actions(uint32_t now)
{
    const hmi_product_t *p = config.product;
    hmi_action_t a;
    while (rt_mq_recv(&actions, &a, sizeof(a), 0) == RT_EOK)
    {
        bool applied = a.kind == HMI_ACTION_PRODUCT
                           ? p->local_action && p->local_action(&config.core->snapshot, &a, now)
                           : hmi_core_action(config.core, &a);
        if (!applied)
        {
            hmi_diag_increment(&actions_rejected);
            LOG_W("action kind=%u id=%u rejected", (unsigned)a.kind, (unsigned)a.id);
        }
        else if (config.action_applied)
        {
            config.action_applied(now);
        }
    }
}
static void periodic_poll(uint32_t now)
{
    const hmi_product_t *p = config.product;
    for (size_t i = 0u; i < p->periodic_count; ++i)
    {
        const hmi_periodic_frame_t *d = &p->periodic[i];
        hmi_can_frame_t frame;
        if (hmi_periodic_due(&periodic_deadline[i], d, &config.core->snapshot, now, true,
                             periodic_in_flight[i], &frame))
        {
            periodic_in_flight[i] = true;
            if (!tx_enqueue(&frame, now + d->period_ms, true, (uint8_t)i, NULL))
                periodic_in_flight[i] = false;
        }
    }
}
static void copy_traces(void)
{
    const hmi_trace_t *source = &app_diag.trace;
    uint32_t count = source->sequence - trace_cursor;
    if (count > source->count)
        count = source->count;
    /* 每次最多复制 16 条，其余留给下一轮。 */
    if (count > 16u)
        count = 16u;
    uint32_t pending = source->sequence - trace_cursor;
    if (pending > source->count)
        pending = source->count;
    uint32_t index = (source->write_index + HMI_TRACE_CAPACITY - pending) % HMI_TRACE_CAPACITY;
    trace_cursor = source->sequence - pending + count;
    for (uint32_t i = 0u; i < count; ++i)
    {
        const hmi_trace_entry_t *e = &source->entries[(index + i) % HMI_TRACE_CAPACITY];
        hmi_trace_append(&config.public_diagnostics->trace, e->timestamp_ms, e->module, e->event, e->arg0,
                         e->arg1);
    }
}
static void publish(uint32_t now)
{
    hmi_core_t *core = config.core;
    for (unsigned bus = 0u; bus < HMI_BUS_COUNT; ++bus)
    {
        hmi_diag_can_t *c = &app_diag.data.can[bus];
        c->tx = tx[bus].ok;
        c->tx_error = tx[bus].error;
        c->last_tx_ms = tx[bus].last_ms;
        c->tx_seen = tx[bus].ok != 0u;
    }
    if (!view_ready || (uint32_t)(now - can_diag_at) >= CAN_DIAG_POLL_MS)
    {
        can_diag_at = now;
        hmi_board_can_diagnostics(&app_diag);
        for (unsigned bus = 0u; bus < HMI_BUS_COUNT; ++bus)
            hmi_bb_can(bus, &app_diag.data.can[bus], tx[bus].dropped, tx_full, rx_budget_hits);
    }
    bool copy = !view_ready || published.revision != core->snapshot.revision ||
                published.generation != core->snapshot.generation;
    hmi_debug_lock();
    if (copy)
        view_ready =
            hmi_snapshot_copy(&published, &config.published_storage, &core->snapshot) == HMI_SNAPSHOT_COPIED;
    hmi_diagnostics_t *d = config.public_diagnostics;
    d->data.runtime = app_diag.data.runtime;
    memcpy(d->data.can, app_diag.data.can, sizeof(d->data.can));
    d->data.domain = app_diag.data.domain;
    copy_traces();
    hmi_debug_unlock();
}
static bool open_buses(void)
{
    const hmi_product_t *p = config.product;
    bool used[HMI_BUS_COUNT] = {false};
    for (size_t i = 0u; i < runtime.routes->count; ++i)
        used[runtime.routes->entries[i].bus] = true;
    for (size_t i = 0u; i < p->periodic_count; ++i)
        used[p->periodic[i].frame.bus] = true;
    const uint32_t bitrate = hmi_board_can_bitrate();
    for (unsigned bus = 0u; bus < HMI_BUS_COUNT; ++bus)
    {
        if (!used[bus])
            continue;
        /* Product 可给单条总线指定开机速率；未指定时沿用统一的 can_rate。 */
        uint32_t bus_bitrate = p->bus_bitrate ? p->bus_bitrate((hmi_bus_role_t)bus, bitrate) : 0u;
        if (!bus_bitrate)
            bus_bitrate = bitrate;
        if (!hmi_board_can_open((hmi_bus_role_t)bus, bus_bitrate, &app_diag) ||
            rt_thread_startup(&tx[bus].thread) != RT_EOK)
            return false;
        tx[bus].open = true;
    }
    return true;
}
/* 启动顺序：先收敛设置读取（CAN 速率要在开总线前确定），再开总线，最后进入会话。 */
static bool app_start(void)
{
    hmi_core_t *core = config.core;
    if (config.runtime_init && !config.runtime_init(core))
        return false;
    if (!hmi_board_settings_boot(&core->snapshot) || !open_buses())
        return false;
    restart_session();
    return true;
}
static bool drain_rx(uint32_t start, uint32_t *now)
{
    hmi_can_frame_t frame;
    size_t count = 0u;
    *now = start;
    hmi_bb_phase(HMI_BB_APP, HMI_BB_CAN_READ, 0);
    while (count < RX_BUDGET && (uint32_t)(*now - start) < RX_SLICE_MS && hmi_board_can_read(&frame))
    {
        ++count;
        hmi_bb_phase(HMI_BB_APP, HMI_BB_CAN_DISPATCH, frame.id);
        (void)hmi_runtime_receive(&runtime, &frame);
        *now = hmi_board_now_ms();
        hmi_bb_phase(HMI_BB_APP, HMI_BB_CAN_READ, 0);
    }
    *now = hmi_board_now_ms();
    const uint32_t elapsed = *now - start;
    if (elapsed > rx_max_ms)
        rx_max_ms = elapsed;
    const bool exhausted = count == RX_BUDGET || elapsed >= RX_SLICE_MS;
    if (exhausted)
        ++rx_budget_hits;
    return exhausted;
}
/* 运行时线程一轮：限时收帧 → 设置动作 → Product 业务 → 周期帧 → 背光 → 发布。 */
static bool app_step(void)
{
    hmi_core_t *core = config.core;
    const uint32_t start = hmi_board_now_ms();
    uint32_t now;
    hmi_diagnostics_time(&app_diag, start);
    const bool exhausted = drain_rx(start, &now);
    hmi_diagnostics_time(&app_diag, now);
    hmi_bb_phase(HMI_BB_APP, HMI_BB_ACTIONS, 0);
    apply_actions(now);
    hmi_bb_phase(HMI_BB_APP, HMI_BB_PROTOCOL, 0);
    hmi_runtime_step(&runtime, core, now, true);
    hmi_bb_phase(HMI_BB_APP, HMI_BB_PERIODIC, 0);
    periodic_poll(now);
#ifdef AIC_HMI_BACKLIGHT
    hmi_bb_phase(HMI_BB_APP, HMI_BB_BACKLIGHT, 0);
    (void)hmi_board_backlight_apply(core->snapshot.brightness, now);
#endif
    hmi_bb_phase(HMI_BB_APP, HMI_BB_PUBLISH, 0);
    publish(now);
    ++app_runs;
    hmi_bb_heartbeat(HMI_BB_APP, hmi_board_now_ms());
    hmi_bb_phase(HMI_BB_APP, HMI_BB_IDLE, 0);
    return exhausted;
}
static void app_wait(bool rx_exhausted, uint32_t *active_since)
{
    const uint32_t now = hmi_board_now_ms();
    if (rx_exhausted || (uint32_t)(now - *active_since) >= APP_ACTIVE_BUDGET_MS)
    {
        rt_thread_mdelay(1);
        *active_since = hmi_board_now_ms();
    }
    else if (rt_sem_take(&wake, rt_tick_from_millisecond(APP_POLL_MS)) != RT_EOK)
    {
        *active_since = hmi_board_now_ms();
    }
}
static void app_entry(void *arg)
{
    (void)arg;
    hmi_bb_phase(HMI_BB_APP, HMI_BB_STARTUP, 0);
    if (!app_start())
    {
        LOG_E("runtime startup failed");
        return;
    }
    publish(hmi_board_now_ms());
    started = true;
    LOG_I("runtime started: generation=%u", (unsigned)generation);
    uint32_t active_since = hmi_board_now_ms();
    for (;;)
    {
        (void)rt_sem_control(&wake, RT_IPC_CMD_RESET, RT_NULL);
        const bool rx_exhausted = app_step();
        app_wait(rx_exhausted, &active_since);
    }
}
bool hmi_execution_start(const hmi_execution_config_t *c)
{
    if (initialized || !c || !c->core || !c->product || !c->public_diagnostics ||
        c->product->periodic_count > HMI_BOARD_PERIODIC_SLOTS ||
        (c->product->periodic_count && !c->product->periodic))
        return false;
    for (size_t i = 0u; i < c->product->periodic_count; ++i)
        if (!hmi_periodic_valid(&c->product->periodic[i]))
            return false;
    config = *c;
    hmi_diagnostics_init(&app_diag);
    hmi_core_bind_diagnostics(c->core, &app_diag);
    if (!hmi_runtime_init(&runtime, c->product, hmi_core_apply, c->core))
        return false;
    hmi_runtime_bind_tx(&runtime, &runtime_tx);
    hmi_runtime_bind_diagnostics(&runtime, &app_diag);
    /* 公共诊断的 Domain 查询读发布副本，与 UI 看到的是同一份。 */
    config.public_diagnostics->domain = &published;
    if (rt_sem_init(&wake, "hmi_rx", 0, RT_IPC_FLAG_FIFO) != RT_EOK ||
        rt_mq_init(&actions, "ui_action", action_pool, sizeof(hmi_action_t), sizeof(action_pool),
                   RT_IPC_FLAG_FIFO) != RT_EOK)
        return false;
    for (unsigned i = 0u; i < HMI_BUS_COUNT; ++i)
    {
        tx_worker_t *t = &tx[i];
        if (rt_sem_init(&t->wake, i ? "tx1_wake" : "tx0_wake", 0, RT_IPC_FLAG_FIFO) != RT_EOK ||
            rt_mq_init(&t->ordinary, i ? "can1_tx" : "can0_tx", t->ordinary_pool, sizeof(tx_message_t),
                       sizeof(t->ordinary_pool), RT_IPC_FLAG_FIFO) != RT_EOK ||
            rt_mq_init(&t->urgent, i ? "can1_bulk" : "can0_bulk", t->urgent_pool, sizeof(tx_message_t),
                       sizeof(t->urgent_pool), RT_IPC_FLAG_FIFO) != RT_EOK ||
            rt_thread_init(&t->thread, i ? "hmi_tx1" : "hmi_tx0", tx_entry, t, t->stack, sizeof(t->stack), 18,
                           5) != RT_EOK)
            return false;
    }
    /* 协议调度可抢占 TX 积压，避免普通帧占满发送时间片时推迟控制帧入队。 */
    if (rt_thread_init(&app_thread, "hmi_app", app_entry, NULL, app_stack, sizeof(app_stack), 17, 5) !=
        RT_EOK)
        return false;
    hmi_board_can_wake(&wake);
    initialized = true;
    return rt_thread_startup(&app_thread) == RT_EOK;
}
bool hmi_execution_action(void *context, const hmi_action_t *action)
{
    (void)context;
    if (!action || !started || rt_mq_send(&actions, action, sizeof(*action)) != RT_EOK)
        return false;
    (void)rt_sem_release(&wake);
    return true;
}
bool hmi_execution_present(hmi_snapshot_t *snapshot, const hmi_core_storage_t *storage)
{
    if (!started || !snapshot || !storage)
        return false;
    hmi_debug_lock();
    bool ok = view_ready;
    if (ok && (!snapshot->catalog || snapshot->revision != published.revision ||
               snapshot->generation != published.generation))
        ok = hmi_snapshot_copy(snapshot, storage, &published) == HMI_SNAPSHOT_COPIED;
    hmi_debug_unlock();
    return ok;
}
void hmi_execution_ui_diagnostics(const hmi_diagnostics_t *d)
{
    if (!initialized || !d)
        return;
    hmi_debug_lock();
    config.public_diagnostics->data.ui = d->data.ui;
    config.public_diagnostics->data.touch = d->data.touch;
    hmi_debug_unlock();
    /* UI 线程每轮发布一次；这个计数是看门狗的 UI 心跳，不借用刷屏计数。 */
    ++ui_ticks;
    hmi_bb_heartbeat(HMI_BB_UI, hmi_board_now_ms());
    ui_presented = true;
}
bool hmi_execution_liveness(hmi_execution_liveness_t *out)
{
    if (!out || !started)
        return false;
    *out = (hmi_execution_liveness_t){.app_runs = app_runs,
                                      .ui_ticks = ui_ticks,
                                      .expect_ui = config.product->supervise_ui && ui_presented};
    return true;
}
static int hmi_exec(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    if (!initialized)
        return -RT_ERROR;
    rt_kprintf("runtime started=%u generation=%u runs=%u\n", started, (unsigned)generation,
               (unsigned)app_runs);
    rt_kprintf("actions rejected=%u tx queue_full=%u\n", (unsigned)actions_rejected, (unsigned)tx_full);
    rt_kprintf("rx budget_hits=%u max_ms=%u\n", (unsigned)rx_budget_hits, (unsigned)rx_max_ms);
    for (unsigned bus = 0u; bus < HMI_BUS_COUNT; ++bus)
        if (tx[bus].open)
            rt_kprintf("tx bus=%u ok=%u error=%u dropped=%u attempts=%u completions=%u max_ms=%u\n", bus,
                       (unsigned)tx[bus].ok, (unsigned)tx[bus].error, (unsigned)tx[bus].dropped,
                       (unsigned)tx[bus].attempts, (unsigned)tx[bus].completions,
                       (unsigned)tx[bus].call_max_ms);
    for (size_t i = 0u; i < config.product->periodic_count; ++i)
        rt_kprintf("periodic entry=%u id=0x%x period_ms=%u missed=%u late_max_ms=%u in_flight=%u\n",
                   (unsigned)i, (unsigned)config.product->periodic[i].frame.id,
                   (unsigned)config.product->periodic[i].period_ms, (unsigned)periodic_deadline[i].missed,
                   (unsigned)periodic_deadline[i].late_ms, periodic_in_flight[i]);
    return RT_EOK;
}
MSH_CMD_EXPORT(hmi_exec, Runtime thread and CAN transmit counters);
