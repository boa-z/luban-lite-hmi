/* SPDX-License-Identifier: Apache-2.0 */
#define LOG_TAG "hmi.can"
#define LOG_LVL LOG_LVL_INFO
#include "platform/rtthread/hmi_blackbox.h"
#include "platform/rtthread/hmi_board_port.h"
#include <rtdevice.h>
#include <rtthread.h>
#include <string.h>
#include <ulog.h>
#ifdef AIC_CAN_DRV_V10
#include <aic_hal_can.h>
#include <aic_hal_clk.h>
#endif
#ifndef AIC_HMI_CAN0_DEVICE
#define AIC_HMI_CAN0_DEVICE "can0"
#endif
#ifndef AIC_HMI_CAN1_DEVICE
#define AIC_HMI_CAN1_DEVICE "can1"
#endif
static const char *const can_names[HMI_BUS_COUNT] = {AIC_HMI_CAN0_DEVICE, AIC_HMI_CAN1_DEVICE};
#ifdef AIC_CAN_DRV_V10
static unsigned long can_native_base(const char *name, uint32_t *clock_id)
{
    (void)name;
    (void)clock_id;
#ifdef AIC_USING_CAN0
    if (strcmp(name, "can0") == 0)
    {
        if (clock_id)
            *clock_id = CLK_CAN0;
        return CAN0_BASE;
    }
#endif
#ifdef AIC_USING_CAN1
    if (strcmp(name, "can1") == 0)
    {
        if (clock_id)
            *clock_id = CLK_CAN1;
        return CAN1_BASE;
    }
#endif
    return 0;
}
#endif
/* 原生驱动 GET_STATUS 返回累计软件错误数。硬件诊断只读无副作用的寄存器。 */
static void can_log_hardware(unsigned bus, uint32_t requested, const char *reason)
{
#ifdef AIC_CAN_DRV_V10
    uint32_t clock_id = 0;
    const unsigned long base = can_native_base(can_names[bus], &clock_id);
    if (!base)
        return;
    const unsigned long clock = hal_clk_get_freq(clock_id);
    const unsigned btr0 = readl(base + CAN_BTR0_REG) & 0xFFU;
    const unsigned btr1 = readl(base + CAN_BTR1_REG) & 0xFFU;
    const unsigned brp = (btr0 & CAN_BTR0_BRP_MASK) + 1U;
    const unsigned tseg1 = (btr1 & CAN_BTR1_TS1_MASK) + 1U;
    const unsigned tseg2 = ((btr1 & CAN_BTR1_TS2_MASK) >> 4) + 1U;
    const unsigned tq = 1U + tseg1 + tseg2;
    const unsigned sample = 1000U * (1U + tseg1) / tq;
    /* 单行保持简短，避免 ulog 长度限制截掉采样点和 SJW。 */
    LOG_I("bus %u %s: clk=%lu requested=%lu readback=%lu", bus, reason, clock, (unsigned long)requested,
          clock / (2U * brp * tq));
    LOG_I("bus %u timing: BTR0=%02x BTR1=%02x BRP=%u tq=%u sample=%u.%u%% SJW=%u", bus, btr0, btr1, brp, tq,
          sample / 10U, sample % 10U, ((btr0 & CAN_BTR0_SJW_MASK) >> 6) + 1U);
    LOG_I("bus %u hardware: MODE=%02x STAT=%02x TEC=%u REC=%u", bus, (unsigned)readl(base + CAN_MODE_REG),
          (unsigned)readl(base + CAN_STAT_REG), (unsigned)readl(base + CAN_TXERR_REG),
          (unsigned)readl(base + CAN_RXERR_REG));
#else
    (void)bus;
    (void)requested;
    (void)reason;
#endif
}
static rt_device_t can_devices[HMI_BUS_COUNT];
static unsigned next_bus;
static struct rt_semaphore *rx_wake;
/* 控制器恢复由 TX worker 串行执行。只有持续检测到控制器异常才重写模式和位时序。 */
enum
{
    CAN_STUCK_MS = 2000,
    CAN_STATUS_BUS_OFF = 4
};
typedef struct
{
    uint32_t bitrate;     /* 打开或最后一次配置成功的速率 */
    bool stuck;           /* 是否处于连续失败 */
    uint32_t stuck_since; /* 连续失败起点 */
    /* 无原生寄存器可查的端口，复位或改速率后仍允许一次探测写入。 */
    bool probe;
} can_recover_t;
static can_recover_t can_recover[HMI_BUS_COUNT];
static void can_recover_clear(unsigned bus)
{
    can_recover[bus].stuck = false;
    can_recover[bus].probe = false;
}
static bool can_tx_blocked(unsigned bus, bool probe)
{
#ifdef AIC_CAN_DRV_V10
    /* 每次发帧前判断硬件是否已重新入网，避免复位时的单次采样或软件缓存阻塞后续帧。 */
    const unsigned long base = can_native_base(can_names[bus], NULL);
    if (base)
        return (readl(base + CAN_STAT_REG) & (CAN_STAT_BUS | CAN_STAT_TXB)) != CAN_STAT_TXB;
#endif
    if (probe)
        return false;
    struct rt_can_status status = {0};
    return rt_device_control(can_devices[bus], RT_CAN_CMD_GET_STATUS, &status) != RT_EOK ||
           status.errcode == CAN_STATUS_BUS_OFF;
}
static bool can_reset_required(unsigned bus)
{
#ifdef AIC_CAN_DRV_V10
    const unsigned long base = can_native_base(can_names[bus], NULL);
    if (base)
        return (readl(base + CAN_MODE_REG) & CAN_MODE_MASK) != 0;
#endif
    struct rt_can_status status = {0};
    return rt_device_control(can_devices[bus], RT_CAN_CMD_GET_STATUS, &status) == RT_EOK &&
           status.errcode == CAN_STATUS_BUS_OFF;
}
static void can_recover_failed(unsigned bus)
{
    can_recover_t *r = &can_recover[bus];
    if (!can_reset_required(bus))
    {
        r->stuck = false;
        return;
    }
    const uint32_t now = hmi_board_now_ms();
    if (!r->stuck)
    {
        r->stuck = true;
        r->stuck_since = now;
        return;
    }
    const uint32_t stuck_ms = (uint32_t)(now - r->stuck_since);
    if (stuck_ms < CAN_STUCK_MS)
        return;
    r->stuck_since = now;
    can_log_hardware(bus, r->bitrate, "before reset");
    /* 累计软件错误数与上面的硬件 TEC/REC 分开输出；ACK 为零不能证明链路有应答。 */
    struct rt_can_status status = {0};
    if (rt_device_control(can_devices[bus], RT_CAN_CMD_GET_STATUS, &status) == RT_EOK)
        LOG_W("bus %u before reset: state=%u txerr=%u rxerr=%u ack=%u bit=%u form=%u stuff=%u crc=%u", bus,
              (unsigned)status.errcode, (unsigned)status.snderrcnt, (unsigned)status.rcverrcnt,
              (unsigned)status.ackerrcnt, (unsigned)status.biterrcnt, (unsigned)status.formaterrcnt,
              (unsigned)status.bitpaderrcnt, (unsigned)status.crcerrcnt);
    /* 先清模式位再重写位时序：SET_MODE 清掉 SLEEP/LISTEN/SELFTEST/RST，
     * SET_BAUD 内部执行 set_reset_mode + 写 BTR + release_reset_mode；只做后者清不掉模式位。 */
    if (rt_device_control(can_devices[bus], RT_CAN_CMD_SET_MODE, (void *)(uintptr_t)RT_CAN_MODE_NORMAL) ==
            RT_EOK &&
        rt_device_control(can_devices[bus], RT_CAN_CMD_SET_BAUD, (void *)(uintptr_t)r->bitrate) == RT_EOK)
    {
        r->probe = true; /* 复位后放行一次探测写入。 */
        hmi_bb_can_recovered(bus, true);
        LOG_W("bus %u stuck %u ms, controller reset at %u bit/s", (unsigned)bus, (unsigned)stuck_ms,
              (unsigned)r->bitrate);
    }
    else
    {
        hmi_bb_can_recovered(bus, false);
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
    /* CAN0 的 HAL 初始化保持复位，必须先写位时序，避免 NORMAL 提前解除复位。 */
    const bool primary = strcmp(can_names[bus], "can0") == 0;
    if ((!primary &&
         rt_device_control(dev, RT_CAN_CMD_SET_MODE, (void *)(uintptr_t)RT_CAN_MODE_NORMAL) != RT_EOK) ||
        rt_device_control(dev, RT_CAN_CMD_SET_BAUD, (void *)(uintptr_t)bitrate) != RT_EOK ||
        (primary &&
         rt_device_control(dev, RT_CAN_CMD_SET_MODE, (void *)(uintptr_t)RT_CAN_MODE_NORMAL) != RT_EOK) ||
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
    can_log_hardware((unsigned)bus, bitrate, "opened");
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
    can_recover_t *recover = &can_recover[bus];
    if (can_tx_blocked(bus, recover->probe))
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
    recover->probe = false; /* 探测写只放行一次，成功与否都要收回。 */
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
    /* 改速率也会进出复位；与看门狗复位相同，不能被滞后的 BUS_OFF 缓存挡住首次写入。 */
    can_recover[bus].probe = true;
    LOG_I("bus %u configured at %lu bit/s", (unsigned)bus, (unsigned long)bitrate);
    can_log_hardware((unsigned)bus, bitrate, "configured");
    return true;
}
bool hmi_board_can_stop(hmi_bus_role_t bus)
{
    if ((unsigned)bus >= HMI_BUS_COUNT || !can_devices[bus] ||
        rt_device_control(can_devices[bus], RT_CAN_CMD_SET_MODE, (void *)(uintptr_t)RT_CAN_MODE_LISTEN) !=
            RT_EOK)
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
            diag->data.can[bus].bitrate = can_recover[bus].bitrate;
            diag->data.can[bus].rx_drop = status.dropedrcvpkg;
            diag->data.can[bus].rx_error = status.rcverrcnt;
            hmi_bb_can_status(bus, status.errcode, status.snderrcnt, status.ackerrcnt);
        }
    }
}
