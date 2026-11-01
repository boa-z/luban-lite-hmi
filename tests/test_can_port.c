/* 原生发送完成语义、BUS_OFF 自动恢复边界、速率与脱离总线行为。 */
#include "platform/rtthread/hmi_can_port.c"
#include <stdio.h>
#include <stdlib.h>
#define CHECK(x)                                                                                             \
    do                                                                                                       \
    {                                                                                                        \
        if (!(x))                                                                                            \
        {                                                                                                    \
            fprintf(stderr, "CAN port %d: %s\n", __LINE__, #x);                                              \
            exit(1);                                                                                         \
        }                                                                                                    \
    } while (0)
static struct rt_device device;
static unsigned opens, baudCalls, mode, delays, writes;
static uint32_t configured;
static size_t writeSize;
static bool failStatus, failRate;
static unsigned errorCode;
uint32_t hmi_board_now_ms(void)
{
    return 0;
}
rt_device_t rt_device_find(const char *name)
{
    CHECK(!strcmp(name, "can0"));
    return &device;
}
rt_err_t rt_device_open(rt_device_t dev, int flags)
{
    CHECK(dev == &device && flags == 3);
    ++opens;
    return RT_EOK;
}
rt_err_t rt_device_close(rt_device_t dev)
{
    (void)dev;
    return RT_EOK;
}
rt_err_t rt_device_set_rx_indicate(rt_device_t dev, rt_err_t (*cb)(rt_device_t, size_t))
{
    CHECK(dev == &device && cb);
    return RT_EOK;
}
rt_err_t rt_device_control(rt_device_t dev, int cmd, void *arg)
{
    CHECK(dev == &device);
    if (cmd == RT_CAN_CMD_SET_BAUD)
    {
        ++baudCalls;
        configured = (uint32_t)(uintptr_t)arg;
        return failRate ? -RT_ERROR : RT_EOK;
    }
    if (cmd == RT_CAN_CMD_SET_MODE)
    {
        mode = (unsigned)(uintptr_t)arg;
        return RT_EOK;
    }
    if (cmd == RT_CAN_CMD_GET_STATUS)
    {
        ((struct rt_can_status *)arg)->errcode = errorCode;
        return failStatus ? -RT_ERROR : RT_EOK;
    }
    return RT_EOK;
}
size_t rt_device_write(rt_device_t dev, int pos, const void *message, size_t size)
{
    (void)pos;
    CHECK(dev == &device && size == sizeof(struct rt_can_msg));
    const struct rt_can_msg *f = message;
    CHECK(f->id == 0x123 && f->len == 8 && f->hdr == -1);
    ++writes;
    return writeSize;
}
void rt_thread_mdelay(int ms)
{
    delays += (unsigned)ms;
}
rt_err_t rt_sem_release(struct rt_semaphore *s)
{
    (void)s;
    return RT_EOK;
}
int main(void)
{
    CHECK(hmi_board_can_open(HMI_BUS_CAN0, 500000, NULL) && opens == 1 && baudCalls == 1);
    hmi_can_frame_t f = {.bus = HMI_BUS_CAN0, .id = 0x123, .size = 8};
    writeSize = sizeof(struct rt_can_msg);
    CHECK(hmi_board_can_send(&f));
    CHECK(writes == 1);
    writeSize = 0;
    errorCode = 0;
    CHECK(!hmi_board_can_send(&f) && writes == 2); /* 正常状态也不能把 0 解释成成功。 */
    writeSize = 1;
    CHECK(!hmi_board_can_send(&f) && writes == 3);
    failStatus = true;
    CHECK(!hmi_board_can_send(&f) && writes == 3);
    failStatus = false;
    writeSize = sizeof(struct rt_can_msg);
    errorCode = 4;
    for (unsigned i = 0; i < 20; ++i)
        CHECK(!hmi_board_can_send(&f));
    CHECK(writes == 3 && baudCalls == 1 && !delays); /* 不重复硬件写、改速率或等待恢复。 */
    errorCode = 0;
    CHECK(hmi_board_can_send(&f) && writes == 4); /* 原生恢复后下一帧可直接发送。 */
    CHECK(hmi_board_can_configure(HMI_BUS_CAN0, 250000) && configured == 250000);
    for (errorCode = 1; errorCode <= 2; ++errorCode)
        CHECK(hmi_board_can_send(&f)); /* warning/passive 不等于 BUS_OFF。 */
    CHECK(baudCalls == 2 && !delays);
    failRate = true;
    CHECK(!hmi_board_can_configure(HMI_BUS_CAN0, 125000));
    failRate = false;
    CHECK(hmi_board_can_stop(HMI_BUS_CAN0) && mode == 1);
    puts("PASS: native TX result, BUS_OFF suppression/recovery, no reset/backoff and offline mode");
    return 0;
}
