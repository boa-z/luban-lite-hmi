/* SPDX-License-Identifier: Apache-2.0 */
#define LOG_TAG "hmi.exec"
#define LOG_LVL LOG_LVL_INFO
#include "platform/rtthread/hmi_execution_port.h"
#include "contracts/hmi_time.h"
#include "core/hmi_snapshot.h"
#include "hmi_display_version.h"
#include "platform/rtthread/debug/hmi_debug_console.h"
#include "platform/rtthread/hmi_board_port.h"
#include "platform/rtthread/hmi_board_settings.h"
#include "platform/rtthread/hmi_nvm_port.h"
#include "runtime/hmi_periodic.h"
#include "runtime/hmi_runtime.h"
#ifdef HMI_ENABLE_CAN_UPDATE
#include "platform/rtthread/hmi_update_port.h"
#include "update/hmi_update.h"
#endif
#include <finsh.h>
#include <rtthread.h>
#include <stdio.h>
#include <string.h>
#include <ulog.h>

/* 容量是板端静态预算，不限制 Product 目录大小；周期条目超过预算时启动失败。 */
#ifndef HMI_BOARD_PERIODIC_SLOTS
#define HMI_BOARD_PERIODIC_SLOTS 8u
#endif
#define TX_DEPTH 16u
#define ACTION_DEPTH 8u
#define RX_BUDGET 64u
#define NO_PERIODIC 0xFFu
#define POOL_WORDS(type, count)                                                                              \
    (((RT_ALIGN(sizeof(type), RT_ALIGN_SIZE) + sizeof(void *)) / sizeof(rt_ubase_t)) * (count))

typedef struct
{
    hmi_can_frame_t frame;
    hmi_can_tx_options_t options;
    uint32_t generation, expires_ms;
    uint8_t periodic; /* 周期条目序号；NO_PERIODIC 表示普通帧 */
    bool session;     /* 属于某个会话代次：代次切换后未发出的帧作废 */
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
    unsigned burst;
    uint32_t error_log_ms, error_logged;
    uint32_t requested_bitrate;
    volatile unsigned configure_state; /* 0 空闲，1 待执行，2 成功，3 失败 */
    /* 以下计数只由本总线的发送线程写。 */
    volatile uint32_t ok, error, dropped, last_ms;
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
static hmi_update_view_t published_update;
static bool view_ready, view_normal;
/* 运行时线程写、其他线程只读的字宽状态。 */
static volatile bool initialized, started, maintenance, ui_healthy;
static volatile uint32_t generation, app_runs, ui_ticks;
static uint32_t suppressed, actions_rejected, tx_full, observed_revision;

static bool tx_enqueue(const hmi_can_frame_t *frame, bool urgent, bool session, uint32_t expires_ms,
                       bool expires, uint8_t periodic, const hmi_can_tx_options_t *options)
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
                      .session = session,
                      .expires = expires};
    if (options)
        m.options = *options;
    if (rt_mq_send(urgent ? &t->urgent : &t->ordinary, &m, sizeof(m)) != RT_EOK)
    {
        hmi_diag_increment(&tx_full);
        return false;
    }
    (void)rt_sem_release(&t->wake);
    return true;
}
bool hmi_execution_can_submit(const hmi_can_frame_t *frame, bool urgent)
{
    return started && tx_enqueue(frame, urgent, !urgent, 0u, false, NO_PERIODIC, NULL);
}
static bool runtime_send(void *context, const hmi_can_frame_t *frame)
{
    (void)context;
    return tx_enqueue(frame, false, true, 0u, false, NO_PERIODIC, NULL);
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
    return options && tx_enqueue(frame, options->priority, true, options->expires_ms, options->expires,
                                 NO_PERIODIC, options);
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
static const hmi_can_tx_port_t runtime_tx = {.send = runtime_send,
                                             .stop = runtime_stop,
                                             .send_checked = runtime_send_checked,
                                             .configure = runtime_configure};

/* 发送线程的一次服务：取一帧并交给驱动；队列为空返回 false。 */
static bool tx_service(tx_worker_t *t)
{
    if (t->stop_requested && !t->stopped)
    {
        t->stopped = hmi_board_can_stop((hmi_bus_role_t)(t - tx));
        if (!t->stopped)
            rt_thread_mdelay(100);
        return true;
    }
    if (!t->stop_requested && __atomic_load_n(&t->configure_state, __ATOMIC_ACQUIRE) == 1u)
    {
        bool ok = hmi_board_can_configure((hmi_bus_role_t)(t - tx), t->requested_bitrate);
        __atomic_store_n(&t->configure_state, ok ? 2u : 3u, __ATOMIC_RELEASE);
        return true;
    }
    tx_message_t m;
    bool got = false;
    /* 升级最多连续四帧；协议显式指定的优先帧不占用该突发配额。 */
    if (t->burst < 4u && rt_mq_recv(&t->urgent, &m, sizeof(m), 0) == RT_EOK)
    {
        got = true;
        t->burst = m.options.priority ? 0u : t->burst + 1u;
    }
    else
    {
        t->burst = 0u;
        got = rt_mq_recv(&t->ordinary, &m, sizeof(m), 0) == RT_EOK ||
              rt_mq_recv(&t->urgent, &m, sizeof(m), 0) == RT_EOK;
    }
    if (!got)
        return false;
    uint32_t now = hmi_board_now_ms();
    bool stale = t->stop_requested || (m.session && m.generation != generation) ||
                 (m.expires && hmi_time_reached(now, m.expires_ms));
    if (!stale && m.options.valid)
        stale = !m.options.valid(m.options.context, m.options.tag);
    /* 检查期间可能发生下线或会话切换，交给驱动前再次读取。 */
    stale = stale || t->stop_requested || (m.session && m.generation != generation) ||
            (m.expires && hmi_time_reached(hmi_board_now_ms(), m.expires_ms));
    bool ok = !stale && hmi_board_can_send(&m.frame);
    if (m.periodic != NO_PERIODIC)
        periodic_in_flight[m.periodic] = false;
    if (stale)
        ++t->dropped;
    else if (ok)
    {
        ++t->ok;
        t->last_ms = hmi_board_now_ms();
    }
    else
    {
        /* 失败不重放、不阻塞后续优先帧；日志最多每秒汇总一次。 */
        ++t->error;
        now = hmi_board_now_ms();
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
    for (;;)
    {
        (void)rt_sem_control(&t->wake, RT_IPC_CMD_RESET, RT_NULL);
        if (!tx_service(t))
            (void)rt_sem_take(&t->wake, RT_WAITING_FOREVER);
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
bool hmi_execution_settings_allowed(void)
{
    return started && !maintenance && hmi_board_nvm_ready();
}
static void apply_actions(uint32_t now)
{
    const hmi_product_t *p = config.product;
    hmi_action_t a;
    while (rt_mq_recv(&actions, &a, sizeof(a), 0) == RT_EOK)
    {
        bool applied = hmi_execution_settings_allowed() &&
                       (a.kind == HMI_ACTION_PRODUCT
                            ? p->local_action && p->local_action(&config.core->snapshot, &a, now)
                            : hmi_core_action(config.core, &a));
        if (!applied)
        {
            hmi_diag_increment(&actions_rejected);
            LOG_W("action kind=%u id=%u rejected", (unsigned)a.kind, (unsigned)a.id);
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
        if (hmi_periodic_due(&periodic_deadline[i], d, &config.core->snapshot, now,
                             !maintenance || d->critical, periodic_in_flight[i], &frame))
        {
            periodic_in_flight[i] = true;
            if (!tx_enqueue(&frame, false, true, now + d->period_ms, true, (uint8_t)i, NULL))
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
static void publish(void)
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
    hmi_board_can_diagnostics(&app_diag);
    hmi_update_view_t update;
#ifdef HMI_ENABLE_CAN_UPDATE
    hmi_board_update_view(&update);
#else
    /* 没有 CAN 升级端点时没人填更新视图，但软件版本页仍要显示本次构建的版本。 */
    memset(&update, 0, sizeof(update));
    (void)snprintf(update.current_version, sizeof(update.current_version), "%s",
                   HMI_FIRMWARE_DISPLAY_VERSION);
#endif
    bool copy = !view_ready || published.revision != core->snapshot.revision ||
                published.generation != core->snapshot.generation;
    hmi_debug_lock();
    if (copy)
        view_ready =
            hmi_snapshot_copy(&published, &config.published_storage, &core->snapshot) == HMI_SNAPSHOT_COPIED;
    published_update = update;
    view_normal = !maintenance;
    hmi_diagnostics_t *d = config.public_diagnostics;
    d->data.runtime = app_diag.data.runtime;
    memcpy(d->data.can, app_diag.data.can, sizeof(d->data.can));
    d->data.domain = app_diag.data.domain;
    d->data.storage = app_diag.data.storage;
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
#ifdef HMI_ENABLE_CAN_UPDATE
    if (p->update)
        used[p->update->bus] = true;
#endif
    const uint32_t bitrate = hmi_board_can_bitrate();
    for (unsigned bus = 0u; bus < HMI_BUS_COUNT; ++bus)
    {
        if (!used[bus])
            continue;
        if (!hmi_board_can_open((hmi_bus_role_t)bus, bitrate, &app_diag) ||
            rt_thread_startup(&tx[bus].thread) != RT_EOK)
            return false;
        tx[bus].open = true;
    }
    return true;
}
/* 启动顺序：先收敛设置读取（CAN 速率要在开总线前确定），再开总线、起升级端点，最后进入会话。 */
static bool app_start(void)
{
    const hmi_product_t *p = config.product;
    hmi_core_t *core = config.core;
    if (p->storage && p->storage->enabled)
    {
        if (!hmi_board_nvm_start(core))
        {
            LOG_E("settings storage failed to start");
            return false;
        }
        while (!hmi_board_nvm_ready())
        {
            hmi_board_nvm_poll(hmi_board_now_ms(), &app_diag.data.storage);
            if (!hmi_board_nvm_ready())
                (void)rt_sem_take(&wake, rt_tick_from_millisecond(5));
        }
    }
    if (!hmi_board_settings_boot(&core->snapshot) || !open_buses())
        return false;
#ifdef HMI_ENABLE_CAN_UPDATE
    if (p->update && !hmi_board_update_start())
        return false;
#endif
    restart_session();
    return true;
}
/* 运行时线程一轮：升级状态 → 收帧分发 → 设置动作 → Product 业务 → 周期帧 → 背光/设置保存 → 发布。
 * 返回本轮读到的帧数。 */
static size_t app_step(uint32_t now)
{
    hmi_core_t *core = config.core;
    ++app_runs;
    hmi_diagnostics_time(&app_diag, now);
    bool paused = false;
#ifdef HMI_ENABLE_CAN_UPDATE
    hmi_board_update_poll(core, ui_healthy);
    paused = hmi_board_update_maintenance() || hmi_board_update_exclusive();
#endif
    if (paused != maintenance)
    {
        maintenance = paused;
        restart_session();
    }
    hmi_can_frame_t frame;
    size_t count = 0u;
    while (count < RX_BUDGET && hmi_board_can_read(&frame))
    {
        ++count;
#ifdef HMI_ENABLE_CAN_UPDATE
        if (hmi_board_update_frame(&frame))
        {
            hmi_diagnostics_can(&app_diag, frame.bus, HMI_CAN_RX, frame.timestamp_ms);
            continue;
        }
#endif
        if (maintenance)
        {
            hmi_diagnostics_can(&app_diag, frame.bus, HMI_CAN_RX, frame.timestamp_ms);
            hmi_diag_increment(&suppressed);
        }
        else
            (void)hmi_runtime_receive(&runtime, &frame);
    }
    apply_actions(now);
    hmi_runtime_step(&runtime, core, now, !maintenance);
    periodic_poll(now);
#ifdef HMI_ENABLE_CAN_UPDATE
    hmi_board_update_protocol();
#endif
#ifdef AIC_HMI_BACKLIGHT
    if (hmi_board_nvm_ready())
        (void)hmi_board_backlight_apply(core->snapshot.brightness, now);
#endif
    /* 快照有变化才编码设置；内容与上次相同时 NVM 服务不会产生写入。 */
    if (core->snapshot.revision != observed_revision)
    {
        observed_revision = core->snapshot.revision;
        (void)hmi_board_nvm_changed(now);
    }
    hmi_board_nvm_poll(now, &app_diag.data.storage);
    publish();
    return count;
}
static void app_entry(void *arg)
{
    (void)arg;
    if (!app_start())
    {
        LOG_E("runtime startup failed");
        return;
    }
    observed_revision = config.core->snapshot.revision;
    publish();
    started = true;
    LOG_I("runtime started: generation=%u", (unsigned)generation);
    for (;;)
    {
        (void)rt_sem_control(&wake, RT_IPC_CMD_RESET, RT_NULL);
        /* 高负载每 64 帧让出一次；空闲时由 RX、UI 动作或期限唤醒（升级时 1 ms，否则 5 ms）。 */
        if (app_step(hmi_board_now_ms()) == RX_BUDGET)
            rt_thread_mdelay(1);
        else
            (void)rt_sem_take(&wake, rt_tick_from_millisecond(
#ifdef HMI_ENABLE_CAN_UPDATE
                                         1
#else
                                         5
#endif
                                         ));
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
    if (!action || !hmi_execution_settings_allowed() ||
        rt_mq_send(&actions, action, sizeof(*action)) != RT_EOK)
        return false;
    (void)rt_sem_release(&wake);
    return true;
}
bool hmi_execution_present(hmi_snapshot_t *snapshot, const hmi_core_storage_t *storage,
                           hmi_update_view_t *update, bool *normal_ui)
{
    if (!started || !snapshot || !storage || !update || !normal_ui)
        return false;
    hmi_debug_lock();
    bool ok = view_ready;
    if (ok && (!snapshot->catalog || snapshot->revision != published.revision ||
               snapshot->generation != published.generation))
        ok = hmi_snapshot_copy(snapshot, storage, &published) == HMI_SNAPSHOT_COPIED;
    *update = published_update;
    *normal_ui = view_normal;
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
    ui_healthy = d->data.ui.flush_count != 0u;
    /* UI 线程每轮发布一次；这个计数是看门狗的 UI 心跳，不借用刷屏计数。 */
    ++ui_ticks;
}
bool hmi_execution_liveness(hmi_execution_liveness_t *out)
{
    if (!out || !started)
        return false;
    *out = (hmi_execution_liveness_t){.app_runs = app_runs, .ui_ticks = ui_ticks};
    return true;
}
static int hmi_exec(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    if (!initialized)
        return -RT_ERROR;
    rt_kprintf("runtime started=%u maintenance=%u generation=%u runs=%u\n", started, maintenance,
               (unsigned)generation, (unsigned)app_runs);
    rt_kprintf("actions rejected=%u rx suppressed=%u tx queue_full=%u\n", (unsigned)actions_rejected,
               (unsigned)suppressed, (unsigned)tx_full);
    for (unsigned bus = 0u; bus < HMI_BUS_COUNT; ++bus)
        if (tx[bus].open)
            rt_kprintf("tx bus=%u ok=%u error=%u dropped=%u\n", bus, (unsigned)tx[bus].ok,
                       (unsigned)tx[bus].error, (unsigned)tx[bus].dropped);
    for (size_t i = 0u; i < config.product->periodic_count; ++i)
        rt_kprintf("periodic entry=%u id=0x%x period_ms=%u missed=%u late_max_ms=%u in_flight=%u\n",
                   (unsigned)i, (unsigned)config.product->periodic[i].frame.id,
                   (unsigned)config.product->periodic[i].period_ms, (unsigned)periodic_deadline[i].missed,
                   (unsigned)periodic_deadline[i].late_ms, periodic_in_flight[i]);
    return RT_EOK;
}
MSH_CMD_EXPORT(hmi_exec, Runtime thread and CAN transmit counters);
