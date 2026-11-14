/* SPDX-License-Identifier: Apache-2.0 */
#define LOG_TAG "hmi.can"
#define LOG_LVL LOG_LVL_INFO
#include "platform/rtthread/hmi_board_port.h"
#include <rtdevice.h>
#include <rtthread.h>
#include <string.h>
#include <ulog.h>
#ifndef AIC_HMI_CAN0_DEVICE
#define AIC_HMI_CAN0_DEVICE "can0"
#endif
#ifndef AIC_HMI_CAN1_DEVICE
#define AIC_HMI_CAN1_DEVICE "can1"
#endif
static const char *const can_names[HMI_BUS_COUNT] = {AIC_HMI_CAN0_DEVICE, AIC_HMI_CAN1_DEVICE};
static rt_device_t can_devices[HMI_BUS_COUNT];
static unsigned next_bus;
static struct rt_semaphore *rx_wake;
/* 卡死看门狗：原生 autoBusoff 只清 MODE.RST，既不重写模式位也不刷新软件状态。
 * 总线持续异常，或热复位后残留 SLEEP/LISTEN 时，控制器会一直不可写且不再产生中断。
 * 这里按"连续失败 ≥ 2 s"复位一次，最小间隔 2 s，任何一次成功都清零；不是按错误次数重试。
 * 复位由该总线的 TX worker 独占执行，与发送、改速率天然串行，不需要额外锁。 */
enum
{
    CAN_STUCK_MS = 2000,      /* 连续失败多久判定为卡死 */
    CAN_RECOVER_MIN_MS = 2000 /* 两次强制复位的最小间隔 */
};
typedef struct
{
    uint32_t bitrate;      /* 打开或最后一次配置成功的速率 */
    bool stuck;            /* 是否处于连续失败 */
    uint32_t stuck_since;  /* 连续失败起点 */
    bool recovered;        /* 本次连续失败内是否已复位过 */
    uint32_t recovered_at; /* 上次强制复位时刻 */
} can_recover_t;
static can_recover_t can_recover[HMI_BUS_COUNT];
static void can_recover_clear(unsigned bus)
{
    can_recover[bus].stuck = false;
    can_recover[bus].recovered = false;
}
static void can_recover_failed(unsigned bus)
{
    can_recover_t *r = &can_recover[bus];
    const uint32_t now = hmi_board_now_ms();
    if (!r->stuck)
    {
        r->stuck = true;
        r->stuck_since = now;
        return;
    }
    const uint32_t stuck_ms = (uint32_t)(now - r->stuck_since);
    if (stuck_ms < CAN_STUCK_MS || (r->recovered && (uint32_t)(now - r->recovered_at) < CAN_RECOVER_MIN_MS))
        return;
    r->recovered = true;
    r->recovered_at = now;
    r->stuck_since = now;
    /* 先清模式位再重写位时序：SET_MODE 清掉 SLEEP/LISTEN/SELFTEST/RST，
     * SET_BAUD 内部执行 set_reset_mode + 写 BTR + release_reset_mode；只做后者清不掉模式位。 */
    if (rt_device_control(can_devices[bus], RT_CAN_CMD_SET_MODE, (void *)(uintptr_t)RT_CAN_MODE_NORMAL) == RT_EOK &&
        rt_device_control(can_devices[bus], RT_CAN_CMD_SET_BAUD, (void *)(uintptr_t)r->bitrate) == RT_EOK)
    {
        LOG_W("bus %u stuck %u ms, controller reset at %u bit/s", (unsigned)bus, (unsigned)stuck_ms,
              (unsigned)r->bitrate);
    }
    else
    {
        LOG_E("bus %u stuck %u ms, controller reset failed", (unsigned)bus, (unsigned)stuck_ms);
    }
}
static rt_err_t rx_indicate(rt_device_t device, rt_size_t size)
{
    (void)device;
    (void)size;
    return rx_wake ? rt_sem_release(rx_wake) : -RT_ERROR;
}
void hmi_board_can_wake(struct rt_semaphore *wake)
{
    rx_wake = wake;
}
bool hmi_board_can_open(hmi_bus_role_t bus, uint32_t bitrate, hmi_diagnostics_t *diag)
{
    if ((unsigned)bus >= HMI_BUS_COUNT)
        return false;
    hmi_diag_can_t *d = diag ? &diag->data.can[bus] : NULL;
    if (d)
        d->available = d->board_available = true;
    rt_device_t dev = rt_device_find(can_names[bus]);
    if (!dev || rt_device_open(dev, RT_DEVICE_FLAG_INT_RX | RT_DEVICE_FLAG_INT_TX) != RT_EOK)
    {
        hmi_diagnostics_can(diag, bus, HMI_CAN_RX_ERROR, hmi_board_now_ms());
        LOG_E("cannot open %s", can_names[bus]);
        return false;
    }
    /* 先强制 NORMAL：热复位不复位外设域，上一次会话留下的 LISTEN/SLEEP 会跨复位保留，
     * hal_can_init() 只动 MODE.RST，清不掉这些模式位。 */
    if (rt_device_control(dev, RT_CAN_CMD_SET_MODE, (void *)(uintptr_t)RT_CAN_MODE_NORMAL) != RT_EOK ||
        rt_device_control(dev, RT_CAN_CMD_SET_BAUD, (void *)(uintptr_t)bitrate) != RT_EOK ||
        rt_device_control(dev, RT_DEVICE_CTRL_SET_INT, NULL) != RT_EOK ||
        rt_device_set_rx_indicate(dev, rx_indicate) != RT_EOK)
    {
        hmi_diagnostics_can(diag, bus, HMI_CAN_RX_ERROR, hmi_board_now_ms());
        LOG_E("cannot configure %s", can_names[bus]);
        (void)rt_device_close(dev);
        return false;
    }
    can_devices[bus] = dev;
    can_recover[bus] = (can_recover_t){.bitrate = bitrate};
    if (d)
    {
        d->open = true;
        d->bitrate = bitrate;
    }
    LOG_I("bus %u on %s opened at %lu bit/s", (unsigned)bus, can_names[bus], (unsigned long)bitrate);
    return true;
}
bool hmi_board_can_read(hmi_can_frame_t *frame)
{
    for (unsigned i = 0; i < HMI_BUS_COUNT; ++i)
    {
        unsigned bus = next_bus;
        next_bus = (next_bus + 1U) % HMI_BUS_COUNT;
        if (!can_devices[bus])
            continue;
        struct rt_can_msg msg = {0};
        msg.hdr = -1;
        if (rt_device_read(can_devices[bus], 0, &msg, sizeof(msg)) != sizeof(msg))
            continue;
        memset(frame, 0, sizeof(*frame));
        frame->bus = (hmi_bus_role_t)bus;
        frame->id = msg.id;
        frame->extended = msg.ide != 0;
        frame->remote = msg.rtr != 0;
        frame->size = msg.len;
        frame->timestamp_ms = hmi_board_now_ms();
        memcpy(frame->data, msg.data, msg.len <= sizeof(frame->data) ? msg.len : sizeof(frame->data));
        return true;
    }
    return false;
}
bool hmi_board_can_send(const hmi_can_frame_t *frame)
{
    if (!frame || (unsigned)frame->bus >= HMI_BUS_COUNT || !can_devices[frame->bus] || frame->size > 8u)
        return false;
    const unsigned bus = (unsigned)frame->bus;
    /* BUS_OFF 期间仍不写硬件邮箱，只累计卡死时间；超过阈值由看门狗复位一次控制器。 */
    struct rt_can_status status = {0};
    if (rt_device_control(can_devices[bus], RT_CAN_CMD_GET_STATUS, &status) != RT_EOK || status.errcode == 4u)
    {
        can_recover_failed(bus);
        return false;
    }
    struct rt_can_msg message = {0};
    message.id = frame->id;
    message.len = frame->size;
    message.ide = frame->extended;
    message.rtr = frame->remote;
    message.hdr = -1;
    memcpy(message.data, frame->data, frame->size);
    /* RT-Thread 只有收到 TX_DONE 才返回完整长度；0 也可能是拒绝、TX_FAIL 或超时。
     * 无法确认是否已上线的帧不当作成功，也不自动重发；后续帧仍按优先级独立服务。 */
    const bool ok = rt_device_write(can_devices[bus], 0, &message, sizeof(message)) == sizeof(message);
    if (ok)
        can_recover_clear(bus);
    else
        can_recover_failed(bus);
    return ok;
}

bool hmi_board_can_configure(hmi_bus_role_t bus, uint32_t bitrate)
{
    if ((unsigned)bus >= HMI_BUS_COUNT || !can_devices[bus] || !bitrate ||
        rt_device_control(can_devices[bus], RT_CAN_CMD_SET_BAUD, (void *)(uintptr_t)bitrate) != RT_EOK)
        return false;
    /* 改速率会进出硬件复位；看门狗按新速率继续，不沿用旧速率。 */
    can_recover[bus].bitrate = bitrate;
    can_recover_clear((unsigned)bus);
    return true;
}
bool hmi_board_can_stop(hmi_bus_role_t bus)
{
    if ((unsigned)bus >= HMI_BUS_COUNT || !can_devices[bus] ||
        rt_device_control(can_devices[bus], RT_CAN_CMD_SET_MODE, (void *)(uintptr_t)RT_CAN_MODE_LISTEN) != RT_EOK)
        return false;
    /* 进入 LISTEN 后不再发送，看门狗没有输入。 */
    can_recover_clear((unsigned)bus);
    return true;
}
void hmi_board_can_diagnostics(hmi_diagnostics_t *diag)
{
    if (!diag)
        return;
    for (unsigned bus = 0u; bus < HMI_BUS_COUNT; ++bus)
    {
        struct rt_can_status status = {0};
        if (can_devices[bus] && rt_device_control(can_devices[bus], RT_CAN_CMD_GET_STATUS, &status) == RT_EOK)
        {
            diag->data.can[bus].rx_drop = status.dropedrcvpkg;
            diag->data.can[bus].rx_error = status.rcverrcnt;
        }
    }
}
