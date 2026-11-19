/* 真实 TX worker：积压、优先级、物理提交前撤销、会话、期限、失败和单向下线。 */
#include "platform/rtthread/hmi_execution_port.c"
#include <stdio.h>
#include <stdlib.h>
#define CHECK(x)                                                                                             \
    do                                                                                                       \
    {                                                                                                        \
        if (!(x))                                                                                            \
        {                                                                                                    \
            fprintf(stderr, "TX line %d: %s\n", __LINE__, #x);                                               \
            exit(1);                                                                                         \
        }                                                                                                    \
    } while (0)
static uint32_t now, activeTag = 1;
static unsigned writes, stops, delays, configurations;
static unsigned filter_calls;
static hmi_can_config_result_t filter_result = HMI_CAN_CONFIG_PENDING;
static size_t rx_pending;
static uint32_t rx_cost_ms;
static uint32_t written[128];
static bool failWrite, failConfigure, expireDuringCheck;
static unsigned settings_boots, resets, action_notifications;
static bool settings_load_ok;
uint32_t hmi_board_now_ms(void)
{
    return now;
}
bool hmi_board_can_send(const hmi_can_frame_t *f)
{
    written[writes++] = f->id;
    return !failWrite;
}
bool hmi_board_can_read(hmi_can_frame_t *frame)
{
    if (!rx_pending)
        return false;
    --rx_pending;
    now += rx_cost_ms;
    *frame = (hmi_can_frame_t){.bus = HMI_BUS_CAN0, .id = 0x123, .timestamp_ms = now};
    return true;
}
bool hmi_board_can_stop(hmi_bus_role_t bus)
{
    CHECK(bus == HMI_BUS_CAN0);
    ++stops;
    return true;
}
bool hmi_board_can_configure(hmi_bus_role_t bus, uint32_t rate)
{
    CHECK(bus == HMI_BUS_CAN0 && rate == 250000);
    ++configurations;
    return !failConfigure;
}
hmi_can_config_result_t hmi_board_can_set_rx_ids(hmi_bus_role_t bus, const hmi_can_rx_ids_t *ids)
{
    CHECK(bus == HMI_BUS_CAN0 && ids->count == 1 && ids->entries[0].id == 0x456);
    ++filter_calls;
    return filter_result;
}
bool hmi_board_settings_boot(const hmi_snapshot_t *snapshot)
{
    CHECK(snapshot->can_rate == HMI_CAN_RATE_250K);
    ++settings_boots;
    return true;
}
uint32_t hmi_board_can_bitrate(void)
{
    return 250000u;
}
bool hmi_board_can_open(hmi_bus_role_t bus, uint32_t bitrate, hmi_diagnostics_t *diag)
{
    (void)bus;
    (void)bitrate;
    (void)diag;
    CHECK(false);
    return false;
}
rt_err_t rt_thread_startup(struct rt_thread *thread)
{
    (void)thread;
    CHECK(false);
    return -RT_ERROR;
}
void rt_thread_mdelay(int ms)
{
    delays += (unsigned)ms;
}
rt_err_t rt_sem_take(struct rt_semaphore *s, int timeout)
{
    CHECK(timeout == (int)APP_POLL_MS);
    if (s->value)
    {
        --s->value;
        return RT_EOK;
    }
    now += (uint32_t)timeout;
    return -RT_ERROR;
}
rt_err_t rt_mq_init(struct rt_messagequeue *q, const char *name, void *pool, size_t size, size_t bytes,
                    int flags)
{
    (void)name;
    (void)flags;
    *q = (struct rt_messagequeue){
        .pool = pool, .size = size, .capacity = bytes / (RT_ALIGN(size, RT_ALIGN_SIZE) + sizeof(void *))};
    return RT_EOK;
}
rt_err_t rt_mq_send(struct rt_messagequeue *q, const void *data, size_t size)
{
    CHECK(size == q->size);
    if (q->count == q->capacity)
        return -RT_EFULL;
    memcpy(q->pool + ((q->head + q->count) % q->capacity) * size, data, size);
    ++q->count;
    return RT_EOK;
}
rt_err_t rt_mq_recv(struct rt_messagequeue *q, void *data, size_t size, int timeout)
{
    (void)timeout;
    CHECK(size == q->size);
    if (!q->count)
        return -RT_ERROR;
    memcpy(data, q->pool + q->head * size, size);
    q->head = (q->head + 1) % q->capacity;
    --q->count;
    return RT_EOK;
}
rt_err_t rt_sem_release(struct rt_semaphore *s)
{
    ++s->value;
    return RT_EOK;
}
static bool valid(void *context, uint32_t tag)
{
    (void)context;
    if (expireDuringCheck)
        now = 20;
    return tag == activeTag;
}
static bool load_settings(hmi_core_t *core)
{
    if (!settings_load_ok)
        return false;
    core->snapshot.can_rate = HMI_CAN_RATE_250K;
    return true;
}
static void reset_product(uint32_t session)
{
    CHECK(session != 0u);
    ++resets;
}
static void notify_action(uint32_t time)
{
    CHECK(time == now);
    ++action_notifications;
}
static void check_no_ui_startup(void)
{
    static const hmi_catalog_t catalog = {0};
    static const hmi_product_t product = {.id = "local", .catalog = &catalog, .app_reset = reset_product};
    hmi_core_t core;
    const hmi_core_storage_t storage = {0};
    CHECK(hmi_core_init(&core, &catalog, &storage));
    CHECK(hmi_runtime_init(&runtime, &product, hmi_core_apply, &core));
    config = (hmi_execution_config_t){
        .product = &product, .core = &core, .runtime_init = load_settings, .action_applied = notify_action};
    CHECK(!app_start() && settings_boots == 0u && resets == 0u);
    settings_load_ok = true;
    CHECK(app_start() && settings_boots == 1u && resets == 1u && core.snapshot.connected);
    CHECK(rt_mq_init(&actions, "action", action_pool, sizeof(hmi_action_t), sizeof(action_pool), 0) ==
          RT_EOK);
    const hmi_action_t set = {.kind = HMI_ACTION_BRIGHTNESS, .value = 64};
    CHECK(rt_mq_send(&actions, &set, sizeof(set)) == RT_EOK);
    apply_actions(now);
    CHECK(core.snapshot.brightness == 64u && action_notifications == 1u);
    config.core = NULL;
}
static void check_app_wait(void)
{
    uint32_t active_since = UINT32_MAX - 2u;
    const uint32_t started_at = active_since;
    const unsigned previous_delays = delays;
    for (uint32_t elapsed = 0; elapsed < APP_ACTIVE_BUDGET_MS; ++elapsed)
    {
        now = started_at + elapsed;
        CHECK(rt_sem_release(&wake) == RT_EOK);
        app_wait(false, &active_since);
        CHECK(active_since == started_at && !wake.value && delays == previous_delays);
    }
    now = started_at + APP_ACTIVE_BUDGET_MS;
    CHECK(rt_sem_release(&wake) == RT_EOK);
    app_wait(false, &active_since);
    CHECK(delays == previous_delays + 1u && active_since == now && wake.value == 1u);
    app_wait(true, &active_since);
    CHECK(delays == previous_delays + 2u && wake.value == 1u);
    CHECK(rt_sem_take(&wake, APP_POLL_MS) == RT_EOK);
    app_wait(false, &active_since);
    CHECK(delays == previous_delays + 2u && now == started_at + APP_ACTIVE_BUDGET_MS + APP_POLL_MS &&
          active_since == now);
}
static void check_rx_budget(void)
{
    runtime.connected = false;
    now = UINT32_MAX;
    rx_pending = RX_BUDGET;
    rx_cost_ms = 1u;
    uint32_t finished;
    CHECK(drain_rx(now, &finished) && rx_pending == RX_BUDGET - RX_SLICE_MS);
    CHECK(finished == now && rx_budget_hits == 1u && rx_max_ms == RX_SLICE_MS);
    rx_pending = RX_BUDGET + 1u;
    rx_cost_ms = 0;
    CHECK(drain_rx(now, &finished) && rx_pending == 1u && rx_budget_hits == 2u);
    CHECK(!drain_rx(now, &finished) && rx_pending == 0u && rx_budget_hits == 2u);
}
int main(void)
{
    tx_worker_t *t = &tx[0];
    t->open = true;
    generation = 1;
    CHECK(rt_mq_init(&t->ordinary, "normal", t->ordinary_pool, sizeof(tx_message_t), sizeof(t->ordinary_pool),
                     0) == RT_EOK);
    CHECK(rt_mq_init(&t->urgent, "priority", t->urgent_pool, sizeof(tx_message_t), sizeof(t->urgent_pool),
                     0) == RT_EOK);
    hmi_can_rx_ids_t ids = {.entries = {{0x456, 0x7FF, false}}, .count = 1};
    CHECK(runtime_set_rx_ids(NULL, HMI_BUS_CAN0, &ids));
    ids.entries[0].id = 0x789;
    CHECK(!runtime_set_rx_ids(NULL, HMI_BUS_CAN0, &ids));
    CHECK(!tx_service(t) && filter_calls == 1 && t->rx_ids_pending);
    filter_result = HMI_CAN_CONFIG_OK;
    CHECK(tx_service(t) && filter_calls == 2 && !t->rx_ids_pending);
    hmi_can_frame_t f = {.bus = HMI_BUS_CAN0, .id = 0x123, .size = 8};
    hmi_can_tx_options_t o = {.valid = valid, .tag = 1};
    CHECK(runtime_send_checked(NULL, &f, &o));
    o.priority = true;
    o.valid = NULL;
    for (unsigned i = 0; i < 8; ++i)
    {
        f.id = 0x130 + i;
        CHECK(runtime_send_checked(NULL, &f, &o));
    }
    for (unsigned i = 0; i < 8; ++i)
    {
        CHECK(tx_service(t));
        CHECK(written[i] == 0x130 + i);
    }
    CHECK(tx_service(t) && written[8] == 0x123);
    CHECK(t->attempts == writes && t->completions == writes);
    const uint32_t initial_attempts = t->attempts;
    writes = 0;
    f.id = 0x123;
    o.priority = false;
    o.valid = valid;
    for (unsigned i = 0; i < TX_DEPTH; ++i)
        CHECK(runtime_send_checked(NULL, &f, &o));
    CHECK(!runtime_send_checked(NULL, &f, &o));
    f.id = 0x124;
    o.priority = true;
    o.valid = NULL;
    CHECK(runtime_send_checked(NULL, &f, &o));
    CHECK(tx_service(t) && writes == 1 && written[0] == 0x124);
    activeTag = 2;
    for (unsigned i = 0; i < TX_DEPTH; ++i)
        CHECK(tx_service(t));
    CHECK(writes == 1 && t->dropped == TX_DEPTH && !tx_service(t));
    o.priority = false;
    o.valid = valid;
    o.tag = 2;
    CHECK(runtime_send_checked(NULL, &f, &o));
    ++generation;
    CHECK(tx_service(t) && writes == 1);
    o.valid = NULL;
    o.expires = true;
    o.expires_ms = 10;
    now = 10;
    CHECK(runtime_send_checked(NULL, &f, &o));
    CHECK(tx_service(t) && writes == 1);
    o.valid = valid;
    o.expires_ms = 20;
    expireDuringCheck = true;
    CHECK(runtime_send_checked(NULL, &f, &o));
    CHECK(tx_service(t) && writes == 1);
    expireDuringCheck = false;
    o.expires = false;
    failWrite = true;
    CHECK(runtime_send_checked(NULL, &f, &o));
    CHECK(tx_service(t));
    CHECK(writes == 2 && t->error == 1 && t->fail_until_ms != 0u && delays == 0);
    /* 退避窗口内普通帧直接丢弃，不再进入驱动；优先帧仍立即尝试。 */
    f.id = 0x125;
    o.priority = true;
    CHECK(runtime_send_checked(NULL, &f, &o));
    CHECK(tx_service(t));
    CHECK(written[2] == 0x125 && writes == 3 && delays == 0);
    o.priority = false;
    f.id = 0x126;
    for (unsigned i = 0; i < 5; ++i)
        CHECK(runtime_send(NULL, &f));
    for (unsigned i = 0; i < 5; ++i)
    {
        CHECK(tx_service(t));
        CHECK(writes == 3); /* 窗口内普通帧不调用驱动。 */
    }
    /* 窗口到期后只试一次；失败后窗口翻倍，后续普通帧再次退避。 */
    now = t->fail_until_ms;
    CHECK(runtime_send(NULL, &f));
    CHECK(tx_service(t) && writes == 4 && t->error == 3 && delays == 0);
    CHECK(runtime_send(NULL, &f));
    CHECK(tx_service(t) && writes == 4);
    /* 成功一次即清除退避，后续普通帧立刻恢复。 */
    failWrite = false;
    now = t->fail_until_ms;
    CHECK(runtime_send(NULL, &f));
    CHECK(tx_service(t) && writes == 5 && t->fail_until_ms == 0u);
    CHECK(runtime_send(NULL, &f));
    CHECK(tx_service(t) && writes == 6 && delays == 0);
    CHECK(t->attempts == initial_attempts + writes && t->completions == t->attempts);
    CHECK(t->error_logged == 1u); /* 同一秒内的连续失败只更新计数。 */
    failWrite = false;
    writes = 3;
    CHECK(runtime_configure(NULL, HMI_BUS_CAN0, 250000) == HMI_CAN_CONFIG_PENDING);
    CHECK(runtime_configure(NULL, HMI_BUS_CAN0, 250000) == HMI_CAN_CONFIG_PENDING && !configurations);
    CHECK(tx_service(t) && configurations == 1);
    CHECK(runtime_configure(NULL, HMI_BUS_CAN0, 250000) == HMI_CAN_CONFIG_OK);
    failConfigure = true;
    CHECK(runtime_configure(NULL, HMI_BUS_CAN0, 250000) == HMI_CAN_CONFIG_PENDING);
    CHECK(tx_service(t));
    CHECK(runtime_configure(NULL, HMI_BUS_CAN0, 250000) == HMI_CAN_CONFIG_FAILED);
    CHECK(runtime_send_checked(NULL, &f, &o));
    CHECK(runtime_stop(NULL, HMI_BUS_CAN0));
    CHECK(!runtime_send_checked(NULL, &f, &o));
    CHECK(tx_service(t) && stops == 1);
    CHECK(tx_service(t) && writes == 3);
    CHECK(!tx_service(t));
    check_app_wait();
    check_no_ui_startup();
    check_rx_budget();
    puts("PASS: CAN worker priority, full queue, cancellation, generation, deadline, failure, configure and "
         "offline, runtime scheduling under sustained receive wakeups");
    return 0;
}
