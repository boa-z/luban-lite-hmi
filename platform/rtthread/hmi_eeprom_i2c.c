#include "platform/rtthread/hmi_eeprom_i2c.h"
#include <rtdevice.h>
#include <rtthread.h>
#include <string.h>
#define LOG_TAG "hmi.nvm"
#define LOG_LVL LOG_LVL_WARNING
#include <ulog.h>

/* 器件地址、容量与页大小来自 Kconfig（Board resources），默认值是 24C512 类两字节寻址器件。 */
#ifndef AIC_HMI_NVM_EEPROM_ADDRESS
#define AIC_HMI_NVM_EEPROM_ADDRESS 0x50
#endif
#ifndef AIC_HMI_NVM_EEPROM_SIZE
#define AIC_HMI_NVM_EEPROM_SIZE 65536
#endif
#ifndef AIC_HMI_NVM_EEPROM_PAGE
#define AIC_HMI_NVM_EEPROM_PAGE 128
#endif
#define EEPROM_ADDRESS (AIC_HMI_NVM_EEPROM_ADDRESS)
#define EEPROM_CAPACITY ((size_t)AIC_HMI_NVM_EEPROM_SIZE)
#define EEPROM_PAGE_SIZE ((size_t)AIC_HMI_NVM_EEPROM_PAGE)
#define EEPROM_WRITE_MS 10
#define EEPROM_SETTLE_MS 1
#define EEPROM_READ_ATTEMPTS 3u

static bool valid_range(size_t address, size_t size)
{
    return address <= EEPROM_CAPACITY && size <= EEPROM_CAPACITY - address;
}

static hmi_io_result_t read_bytes(void *context, size_t address, uint8_t *data, size_t size)
{
    struct rt_i2c_bus_device *bus = context;
    if (!bus)
        return HMI_IO_ERROR;
    if (!valid_range(address, size) || (!data && size))
        return HMI_IO_RANGE;
    if (!size)
        return HMI_IO_OK;
    if (size > EEPROM_PAGE_SIZE)
        return HMI_IO_RANGE;
    uint8_t offset[2] = {(uint8_t)(address >> 8u), (uint8_t)address};
    uint8_t received[EEPROM_PAGE_SIZE];
    for (unsigned attempt = 0u; attempt < EEPROM_READ_ATTEMPTS; ++attempt)
    {
        struct rt_i2c_msg addr = {EEPROM_ADDRESS, RT_I2C_WR, 2, offset};
        struct rt_i2c_msg value = {EEPROM_ADDRESS, RT_I2C_RD, (rt_uint16_t)size, received};
        /* 给上一事务的控制器关闭/STOP 留出间隔；失败只重试完整地址/读取对。 */
        rt_thread_mdelay(EEPROM_SETTLE_MS);
        if (rt_mutex_take(&bus->lock, RT_WAITING_FOREVER) != RT_EOK)
            return HMI_IO_ERROR;
        const rt_size_t sent = rt_i2c_transfer(bus, &addr, 1);
        rt_size_t got = 0u;
        if (sent == 1u)
        {
            /* 地址和连续读取不能被插入其他访问，因此此处短暂持递归总线锁。 */
            rt_thread_mdelay(EEPROM_SETTLE_MS);
            got = rt_i2c_transfer(bus, &value, 1);
        }
        const rt_err_t released = rt_mutex_release(&bus->lock);
        if (released != RT_EOK)
            return HMI_IO_ERROR;
        if (sent == 1u && got == 1u)
        {
            memcpy(data, received, size);
            return HMI_IO_OK;
        }
        LOG_W("EEPROM read addr=0x%04x bytes=%u attempt=%u/%u address=%u read=%u", (unsigned)address,
              (unsigned)size, attempt + 1u, EEPROM_READ_ATTEMPTS, (unsigned)sent, (unsigned)got);
    }
    return HMI_IO_ERROR;
}

static hmi_io_result_t write_page(void *context, size_t address, const uint8_t *data, size_t size)
{
    struct rt_i2c_bus_device *bus = context;
    if (!bus)
        return HMI_IO_ERROR;
    if (!valid_range(address, size) || (!data && size) ||
        size > EEPROM_PAGE_SIZE - address % EEPROM_PAGE_SIZE)
        return HMI_IO_RANGE;
    if (!size)
        return HMI_IO_OK;
    /* 一次传输写完 EEPROM 的整页：上层已按页边界切分。逐字节写会把一条 200 字节的
       设置记录拖成 2~3 秒，密码改完后立刻断电就会退回旧记录。 */
    uint8_t bytes[2u + EEPROM_PAGE_SIZE];
    bytes[0] = (uint8_t)(address >> 8u);
    bytes[1] = (uint8_t)address;
    memcpy(bytes + 2u, data, size);
    struct rt_i2c_msg msg = {EEPROM_ADDRESS, RT_I2C_WR, (rt_uint16_t)(2u + size), bytes};
    /* 写入不重放；短传输交由上层 UNCERTAIN 和显式协调处理。 */
    rt_thread_mdelay(EEPROM_SETTLE_MS);
    const rt_size_t sent = rt_i2c_transfer(bus, &msg, 1);
    /* 即使驱动报告失败，也等待可能已经开始的写周期；此时不持总线锁。 */
    rt_thread_mdelay(EEPROM_WRITE_MS);
    if (sent != 1u)
    {
        LOG_W("EEPROM page write addr=0x%04x bytes=%u messages=%u/1", (unsigned)address, (unsigned)size,
              (unsigned)sent);
        return HMI_IO_ERROR;
    }
    uint8_t actual[EEPROM_PAGE_SIZE];
    const hmi_io_result_t result = read_bytes(context, address, actual, size);
    /* 驱动成功不代表写入成功；WP 或吞掉的 NACK 在读回不一致时立即终止。 */
    if (result != HMI_IO_OK || memcmp(actual, data, size) != 0)
    {
        LOG_W("EEPROM page verify addr=0x%04x bytes=%u result=%u", (unsigned)address, (unsigned)size,
              (unsigned)result);
        return HMI_IO_ERROR;
    }
    return HMI_IO_OK;
}

static hmi_io_result_t ready(void *context)
{
    uint8_t value;
    return read_bytes(context, 0u, &value, 1u);
}

static void wait_ms(void *context, uint32_t ms)
{
    (void)context;
    rt_thread_mdelay((rt_int32_t)ms);
}

bool hmi_eeprom_i2c_bind(hmi_eeprom_port_t *port, struct rt_i2c_bus_device *bus)
{
    if (!port || !bus)
        return false;
    *port = (hmi_eeprom_port_t){bus, read_bytes, write_page, ready, wait_ms};
    return true;
}
