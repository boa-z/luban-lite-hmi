/* 原生发送完成语义、BUS_OFF 抑制、卡死看门狗、速率与脱离总线行为。 */
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
static unsigned opens, baudCalls, modeCalls, mode, delays, writes;
static uint32_t configured, nowMs;
static size_t writeSize;
static bool failStatus, failRate;
static unsigned errorCode;
uint32_t hmi_board_now_ms(void)
{
    return nowMs;
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
        ++modeCalls;
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
    /* 打开时必须先强制 NORMAL 模式，再写速率：热复位残留的 LISTEN/SLEEP 只能这样清掉。 */
    CHECK(hmi_board_can_open(HMI_BUS_CAN0, 500000, NULL) && opens == 1 && modeCalls == 1 && mode == 0 &&
          baudCalls == 1);
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
    CHECK(hmi_board_can_send(&f) && writes == 4); /* 成功一次即清零看门狗。 */

    /* BUS_OFF：不写硬件邮箱；连续 2 s 后复位一次（SET_MODE NORMAL + SET_BAUD），最小间隔 2 s。 */
    errorCode = 4;
    nowMs = 1000;
    CHECK(!hmi_board_can_send(&f) && writes == 4 && baudCalls == 1); /* 进入 BUS_OFF，开始计时。 */
    nowMs = 2999;
    CHECK(!hmi_board_can_send(&f) && writes == 4 && baudCalls == 1); /* 未到阈值不复位。 */
    nowMs = 3000;
    CHECK(!hmi_board_can_send(&f) && writes == 4 && modeCalls == 2 && mode == 0 && baudCalls == 2 &&
          configured == 500000); /* 到阈值，复位一次。 */

    /* 复位后放行一次探测写入：驱动的 errcode 仍是 BUS_OFF（软件状态滞后于硬件），
     * 必须真写一次才能知道控制器是否已恢复。写成功即清零，即使 errcode 还残留 4。 */
    writeSize = sizeof(struct rt_can_msg);
    CHECK(hmi_board_can_send(&f) && writes == 5);  /* 探测写成功。 */
    CHECK(!hmi_board_can_send(&f) && writes == 5); /* 探测只放行一次，收回后重新按 errcode 门禁。 */

    nowMs = 4000;
    CHECK(!hmi_board_can_send(&f) && baudCalls == 2); /* 最小间隔内不重复复位。 */

    /* 探测写失败也要收回，不能反复放行。 */
    writeSize = 0;
    nowMs = 6000;
    CHECK(!hmi_board_can_send(&f) && writes == 5 && modeCalls == 3 && baudCalls == 3); /* 到点复位，本轮不写。 */
    CHECK(!hmi_board_can_send(&f) && writes == 6); /* 探测写一次，失败即收回。 */
    CHECK(!hmi_board_can_send(&f) && writes == 6); /* 不再放行。 */

    /* 非 BUS_OFF 的卡死：门禁不拦，写失败同样计时并复位。 */
    errorCode = 0;
    writeSize = sizeof(struct rt_can_msg);
    nowMs = 7000;
    CHECK(hmi_board_can_send(&f) && writes == 7); /* 原生状态恢复后成功即清零，重新计时。 */
    writeSize = 0;
    nowMs = 8000;
    CHECK(!hmi_board_can_send(&f) && writes == 8); /* 短写计入卡死。 */
    nowMs = 10000;
    CHECK(!hmi_board_can_send(&f) && writes == 9 && modeCalls == 4 && baudCalls == 4);
    CHECK(!hmi_board_can_send(&f) && writes == 10); /* 探测写失败，收回。 */

    /* 改速率本身是复位，看门狗按新速率继续。 */
    writeSize = sizeof(struct rt_can_msg);
    CHECK(hmi_board_can_configure(HMI_BUS_CAN0, 250000) && configured == 250000 && baudCalls == 5);
    for (errorCode = 1; errorCode <= 2; ++errorCode)
        CHECK(hmi_board_can_send(&f)); /* warning/passive 不等于 BUS_OFF。 */
    failRate = true;
    CHECK(!hmi_board_can_configure(HMI_BUS_CAN0, 125000) && baudCalls == 6);
    failRate = false;
    CHECK(hmi_board_can_stop(HMI_BUS_CAN0) && mode == RT_CAN_MODE_LISTEN && modeCalls == 5);
    CHECK(!delays);
    puts("PASS: native TX result, BUS_OFF suppression, stuck watchdog, probe write, rate and listen mode");
    return 0;
}
