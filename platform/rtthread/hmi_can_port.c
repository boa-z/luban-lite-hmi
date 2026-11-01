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
    if (rt_device_control(dev, RT_CAN_CMD_SET_BAUD, (void *)(uintptr_t)bitrate) != RT_EOK ||
        rt_device_control(dev, RT_DEVICE_CTRL_SET_INT, NULL) != RT_EOK ||
        rt_device_set_rx_indicate(dev, rx_indicate) != RT_EOK)
    {
        hmi_diagnostics_can(diag, bus, HMI_CAN_RX_ERROR, hmi_board_now_ms());
        LOG_E("cannot configure %s", can_names[bus]);
        (void)rt_device_close(dev);
        return false;
    }
    can_devices[bus] = dev;
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
    /* AIC HAL 已处理 BUS_OFF 自动恢复；恢复前不重复写满硬件邮箱或反复重置控制器。 */
    struct rt_can_status status = {0};
    if (rt_device_control(can_devices[frame->bus], RT_CAN_CMD_GET_STATUS, &status) != RT_EOK ||
        status.errcode == 4u)
        return false;
    struct rt_can_msg message = {0};
    message.id = frame->id;
    message.len = frame->size;
    message.ide = frame->extended;
    message.rtr = frame->remote;
    message.hdr = -1;
    memcpy(message.data, frame->data, frame->size);
    /* RT-Thread 只有收到 TX_DONE 才返回完整长度；0 也可能是拒绝、TX_FAIL 或超时。
     * 无法确认是否已上线的帧不当作成功，也不自动重发；后续帧仍按优先级独立服务。 */
    return rt_device_write(can_devices[frame->bus], 0, &message, sizeof(message)) == sizeof(message);
}

bool hmi_board_can_configure(hmi_bus_role_t bus, uint32_t bitrate)
{
    if ((unsigned)bus >= HMI_BUS_COUNT || !can_devices[bus] ||
        rt_device_control(can_devices[bus], RT_CAN_CMD_SET_BAUD, (void *)(uintptr_t)bitrate) != RT_EOK)
        return false;
    return true;
}
bool hmi_board_can_stop(hmi_bus_role_t bus)
{
    return (unsigned)bus < HMI_BUS_COUNT && can_devices[bus] &&
           rt_device_control(can_devices[bus], RT_CAN_CMD_SET_MODE, (void *)(uintptr_t)RT_CAN_MODE_LISTEN) ==
               RT_EOK;
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
