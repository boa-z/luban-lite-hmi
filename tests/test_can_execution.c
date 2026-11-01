/* 真实 TX worker：积压、优先级、物理提交前撤销、会话、期限、失败和单向下线。 */
#include "platform/rtthread/hmi_execution_port.c"
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
static uint32_t written[128];
static bool failWrite, failConfigure, expireDuringCheck;
uint32_t hmi_board_now_ms(void)
{
    return now;
}
bool hmi_board_can_send(const hmi_can_frame_t *f)
{
    written[writes++] = f->id;
    return !failWrite;
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
void rt_thread_mdelay(int ms)
{
    delays += (unsigned)ms;
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
int main(void)
{
    tx_worker_t *t = &tx[0];
    t->open = true;
    generation = 1;
    CHECK(rt_mq_init(&t->ordinary, "normal", t->ordinary_pool, sizeof(tx_message_t), sizeof(t->ordinary_pool),
                     0) == RT_EOK);
    CHECK(rt_mq_init(&t->urgent, "priority", t->urgent_pool, sizeof(tx_message_t), sizeof(t->urgent_pool),
                     0) == RT_EOK);
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
    CHECK(writes == 2 && t->error == 1 && delays == 0);
    f.id = 0x125;
    o.priority = true;
    CHECK(runtime_send_checked(NULL, &f, &o));
    CHECK(tx_service(t));
    CHECK(written[2] == 0x125 && delays == 0);
    /* 未使用 checked API 的既有 Product 同样没有整队退避；50 ms 时优先帧先于普通积压。 */
    failWrite = true;
    f.id = 0x126;
    CHECK(runtime_send(NULL, &f));
    CHECK(tx_service(t) && delays == 0);
    for (unsigned i = 0; i < 10; ++i)
        CHECK(runtime_send(NULL, &f));
    now += 50;
    f.id = 0x127;
    CHECK(runtime_send_checked(NULL, &f, &o));
    CHECK(tx_service(t) && written[4] == 0x127 && delays == 0);
    now += 1000;
    while (tx_service(t))
        ;
    CHECK(writes == 15 && t->error == 14 && delays == 0);
    CHECK(t->error_logged == 5); /* 同一毫秒的后续失败只更新计数。 */
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
    puts("PASS: CAN worker priority, full queue, cancellation, generation, deadline, failure, configure and "
         "offline");
    return 0;
}
