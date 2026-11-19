#include <rtthread.h>

#include "platform/rtthread/hmi_blackbox.h"

#include <dfs_fs.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#ifdef RT_USING_FINSH
#include <finsh.h>
#endif
#ifdef AIC_WRI_DRV
#include <hal_wri.h>
#endif

#include "hmi_display_version.h"

#define BB_MAGIC 0x31424248u
#define BB_SCHEMA 1u
#define BB_SECTOR 512u
#define BB_SLOTS 4096u
#define BB_PERIOD_MS 1000u
#define BB_PATH AIC_HMI_BLACKBOX_MOUNT "/hmi-bb-v1.bin"

typedef struct
{
    uint32_t stage, entered_ms, detail, longest_ms, longest_stage;
    uint32_t heartbeat, heartbeat_ms, completed, last_detail, result, code;
} bb_owner_t;

typedef struct
{
    uint32_t at_ms, detail, code;
} bb_failure_t;

typedef struct
{
    uint32_t available, sampled_ms, state, tx_errors, ack_errors, recovery_ok, recovery_failed;
    uint32_t rx, tx, rx_drop, tx_drop, rx_error, tx_error, tx_queue_full, rx_budget_hits;
    uint32_t last_rx_ms, last_tx_ms;
} bb_can_t;

typedef struct
{
    uint32_t magic, schema, sequence, boot_sequence, uptime_ms, reset_hw, reset_sdk;
    char firmware[32];
    bb_owner_t owners[HMI_BB_OWNERS];
    uint32_t wdt_feed_ms, wdt_state, wdt_state_ms, wdt_channel, wdt_feed_errors;
    bb_can_t can[HMI_BUS_COUNT];
    bb_failure_t sdo_failure, nvm_failure;
} bb_data_t;

typedef struct
{
    bb_data_t data;
    uint8_t reserved[BB_SECTOR - sizeof(bb_data_t) - sizeof(uint32_t)];
    uint32_t crc;
} bb_record_t;

_Static_assert(sizeof(bb_record_t) == BB_SECTOR, "Black box record must fit one logical sector");
_Static_assert(sizeof(bb_data_t) == 504u, "Black box data must match the version 1 decoder");

static bb_data_t live;
static bb_record_t record;
static struct rt_thread worker;
static rt_ubase_t stack[4096u / sizeof(rt_ubase_t)];
static volatile uint32_t written, last_sync_ms, io_errors;

static uint32_t now_ms(void)
{
    return (uint32_t)rt_tick_get_millisecond();
}

static void enter_phase(bb_owner_t *item, uint32_t stage, uint32_t detail, uint32_t now)
{
    const uint32_t duration = now - item->entered_ms;
    if (item->stage > HMI_BB_IDLE && duration > item->longest_ms)
    {
        item->longest_ms = duration;
        item->longest_stage = item->stage;
    }
    item->stage = stage;
    item->entered_ms = now;
    item->detail = detail;
}

void hmi_bb_phase(hmi_bb_owner_t owner, hmi_bb_stage_t stage, uint32_t detail)
{
    const rt_base_t irq = rt_hw_interrupt_disable();
    enter_phase(&live.owners[owner], stage, detail, now_ms());
    rt_hw_interrupt_enable(irq);
}

void hmi_bb_complete(hmi_bb_owner_t owner, uint32_t result, uint32_t code)
{
    const rt_base_t irq = rt_hw_interrupt_disable();
    bb_owner_t *item = &live.owners[owner];
    ++item->completed;
    item->last_detail = item->detail;
    item->result = result;
    item->code = code;
    const uint32_t now = now_ms();
    if (result != 0u && owner == HMI_BB_SDO)
        live.sdo_failure = (bb_failure_t){now, item->detail, code != 0u ? code : result};
    if (result != 0u && owner == HMI_BB_NVM)
        live.nvm_failure = (bb_failure_t){now, item->detail, code};
    enter_phase(item, HMI_BB_IDLE, 0, now);
    rt_hw_interrupt_enable(irq);
}

void hmi_bb_heartbeat(hmi_bb_owner_t owner, uint32_t now)
{
    const rt_base_t irq = rt_hw_interrupt_disable();
    ++live.owners[owner].heartbeat;
    live.owners[owner].heartbeat_ms = now;
    rt_hw_interrupt_enable(irq);
}

void hmi_bb_watchdog(uint32_t now, uint32_t state, uint32_t channel)
{
    const rt_base_t irq = rt_hw_interrupt_disable();
    if (state == HMI_BB_WDT_FEED)
        live.wdt_feed_ms = now;
    if (state == HMI_BB_WDT_FEED_ERROR)
        ++live.wdt_feed_errors;
    if (live.wdt_state != state || live.wdt_channel != channel)
    {
        live.wdt_state_ms = now;
        live.wdt_state = state;
        live.wdt_channel = channel;
    }
    rt_hw_interrupt_enable(irq);
}

void hmi_bb_can(unsigned bus, const hmi_diag_can_t *counts, uint32_t tx_drop, uint32_t tx_queue_full,
                uint32_t rx_budget_hits)
{
    const rt_base_t irq = rt_hw_interrupt_disable();
    bb_can_t *can = &live.can[bus];
    can->rx = counts->rx;
    can->tx = counts->tx;
    can->rx_drop = counts->rx_drop;
    can->tx_drop = tx_drop;
    can->rx_error = counts->rx_error;
    can->tx_error = counts->tx_error;
    can->tx_queue_full = tx_queue_full;
    can->rx_budget_hits = rx_budget_hits;
    can->last_rx_ms = counts->last_rx_ms;
    can->last_tx_ms = counts->last_tx_ms;
    rt_hw_interrupt_enable(irq);
}

void hmi_bb_can_status(unsigned bus, uint32_t state, uint32_t tx_errors, uint32_t ack_errors)
{
    const rt_base_t irq = rt_hw_interrupt_disable();
    bb_can_t *can = &live.can[bus];
    can->available = 1;
    can->sampled_ms = now_ms();
    can->state = state;
    can->tx_errors = tx_errors;
    can->ack_errors = ack_errors;
    rt_hw_interrupt_enable(irq);
}

void hmi_bb_can_recovered(unsigned bus, bool success)
{
    const rt_base_t irq = rt_hw_interrupt_disable();
    if (success)
        ++live.can[bus].recovery_ok;
    else
        ++live.can[bus].recovery_failed;
    rt_hw_interrupt_enable(irq);
}

static uint32_t record_crc(void)
{
    const uint8_t *bytes = (const uint8_t *)&record;
    uint32_t crc = UINT32_MAX;
    for (size_t offset = 0; offset < BB_SECTOR - sizeof(record.crc); ++offset)
    {
        crc ^= bytes[offset];
        for (unsigned bit = 0; bit < 8u; ++bit)
            crc = (crc >> 1) ^ ((crc & 1u) ? 0xEDB88320u : 0u);
    }
    return ~crc;
}

static int open_ring(uint32_t *latest, uint32_t *last_boot)
{
    rt_device_t device = rt_device_find(AIC_HMI_BLACKBOX_DEVICE);
    struct dfs_filesystem *fs = device ? dfs_filesystem_lookup(BB_PATH) : RT_NULL;
    if (!fs || fs->dev_id != device)
        return -1;

    const int fd = open(BB_PATH, O_CREAT | O_RDWR, 0);
    if (fd < 0)
        return -1;
    for (uint32_t slot = 0; slot < BB_SLOTS; ++slot)
    {
        const ssize_t size = read(fd, &record, sizeof(record));
        if (size < 0)
            goto failed;
        if (size != (ssize_t)sizeof(record))
        {
            memset(&record, 0, sizeof(record));
            if (lseek(fd, (off_t)slot * BB_SECTOR, SEEK_SET) < 0)
                goto failed;
            for (; slot < BB_SLOTS; ++slot)
            {
                if (write(fd, &record, sizeof(record)) != (ssize_t)sizeof(record))
                    goto failed;
            }
            if (fsync(fd) != 0)
                goto failed;
            break;
        }
        if (record.data.magic == BB_MAGIC && record.data.schema == BB_SCHEMA &&
            record.data.sequence > *latest && (record.data.sequence - 1u) % BB_SLOTS == slot &&
            record.crc == record_crc())
        {
            *latest = record.data.sequence;
            *last_boot = record.data.boot_sequence;
        }
    }
    return fd;

failed:
    (void)close(fd);
    return -1;
}

static bool save_record(int fd, uint32_t sequence, uint32_t boot_sequence)
{
    memset(&record, 0, sizeof(record));
    rt_enter_critical();
    record.data = live;
    rt_exit_critical();
    record.data.magic = BB_MAGIC;
    record.data.schema = BB_SCHEMA;
    record.data.sequence = sequence;
    record.data.boot_sequence = boot_sequence;
    record.data.uptime_ms = now_ms();
    record.crc = record_crc();
    const off_t offset = (off_t)((sequence - 1u) % BB_SLOTS) * BB_SECTOR;
    return lseek(fd, offset, SEEK_SET) == offset &&
           write(fd, &record, sizeof(record)) == (ssize_t)sizeof(record) && fsync(fd) == 0;
}

static void record_loop(void *arg)
{
    (void)arg;
    uint32_t boot_sequence = 0;
    for (;;)
    {
        uint32_t latest = 0;
        uint32_t last_boot = 0;
        const int fd = open_ring(&latest, &last_boot);
        if (fd >= 0)
        {
            if (boot_sequence == 0u || boot_sequence != last_boot)
                boot_sequence = latest + 1u;
            while (save_record(fd, ++latest, boot_sequence))
            {
                ++written;
                last_sync_ms = now_ms();
                rt_thread_mdelay(BB_PERIOD_MS);
            }
            (void)close(fd);
        }
        ++io_errors;
        rt_thread_mdelay(5000);
    }
}

static int hmi_bb_init(void)
{
    live.reset_hw = UINT32_MAX;
    live.reset_sdk = UINT32_MAX;
#ifdef AIC_WRI_DRV
    live.reset_sdk = (uint32_t)aic_get_reboot_reason();
    live.reset_hw = (uint32_t)aic_wr_type_get();
#endif
    rt_strncpy(live.firmware, HMI_FIRMWARE_DISPLAY_VERSION, sizeof(live.firmware) - 1u);
    if (rt_thread_init(&worker, "hmi_bb", record_loop, RT_NULL, stack, sizeof(stack),
                       RT_THREAD_PRIORITY_MAX - 3, 5) != RT_EOK)
        return -RT_ERROR;
    return rt_thread_startup(&worker);
}
INIT_APP_EXPORT(hmi_bb_init);

#ifdef RT_USING_FINSH
static void hmi_blackbox(void)
{
    rt_kprintf("blackbox path=%s synced=%u last_sync_ms=%u unavailable_or_io_errors=%u\n", BB_PATH,
               (unsigned)written, (unsigned)last_sync_ms, (unsigned)io_errors);
}
MSH_CMD_EXPORT(hmi_blackbox, Show TF runtime recorder status);
#endif
