#ifndef HMI_DIAGNOSTICS_H
#define HMI_DIAGNOSTICS_H
#include "contracts/hmi_can_frame.h"
#include "contracts/hmi_domain.h"
#include "diagnostics/hmi_trace.h"
/** @brief 协议运行时计数：收到（accepted）后要么分发成功、要么未路由或解码失败。 */
typedef struct
{
    uint32_t accepted, malformed, unrouted, decode_failed, dispatched, resets;
} hmi_runtime_diagnostics_t;
/** @brief Runtime 对外状态，无私有指针。 */
typedef struct
{
    bool available, connected;
    uint32_t generation;
    size_t adapters;
    hmi_runtime_diagnostics_t counters;
} hmi_diag_runtime_t;
/** @brief 总线统计；available 表示已接入观测，board_available 表示硬件状态已知。 */
typedef struct
{
    bool available, board_available, open, rx_seen, tx_seen;
    uint32_t bitrate, rx, tx, rx_drop, tx_busy, rx_error, tx_error, last_rx_ms, last_tx_ms;
} hmi_diag_can_t;
/** @brief Domain 统计不改变 Domain revision；结构规模来自产品目录。 */
typedef struct
{
    bool available, connected;
    size_t signals, parameters, faults;
    uint32_t generation, revision;
    uint32_t updates, stale_transition, valid_transition, source_switch, error_updates;
} hmi_diag_domain_t;
/** @brief UI 低成本统计，flush_available 区分尚未接入的后端。 */
typedef struct
{
    bool available, flush_available;
    uint32_t present_count, flush_count;
} hmi_diag_ui_t;
/** @brief 复用触摸驱动公开诊断值，禁止查询时触发新的触摸采样。 */
typedef struct
{
    bool available;
    int32_t range_x, range_y, x, y, state;
    uint32_t irq, reads, events, delivered, recovered, empty_reads, invalid_reads;
} hmi_diag_touch_t;
/** @brief 一次统一查询结果；所有 unavailable 模块保持零值。 */
typedef struct
{
    uint32_t uptime_ms;
    hmi_diag_runtime_t runtime;
    hmi_diag_can_t can[HMI_BUS_COUNT];
    hmi_diag_domain_t domain;
    hmi_diag_ui_t ui;
    hmi_diag_touch_t touch;
    uint32_t trace_capacity, trace_count, trace_write_index, trace_overwritten;
} hmi_diag_snapshot_t;
/** @brief 固件身份字符串由构建系统提供，生命周期覆盖程序运行期。 */
typedef struct
{
    const char *product, *platform_revision, *sdk_revision, *lvgl_version, *board, *build_date, *build_time,
        *display_version;
} hmi_build_info_t;
/** @brief 调用者持有的静态诊断实例；字段仅允许 owner 更新，跨线程由宿主锁保护。 */
typedef struct hmi_diagnostics
{
    hmi_diag_snapshot_t data;
    hmi_trace_t trace;
    const hmi_snapshot_t *domain;
} hmi_diagnostics_t;
/** @brief 查询信号的值拷贝；key/unit 指向生命周期覆盖查询的只读产品目录。 */
typedef struct
{
    uint16_t id;
    const char *key, *unit;
    hmi_value_t value;
    uint32_t age_ms, stale_ms;
} hmi_diag_signal_t;
/** @brief 初始化实例，不分配、不阻塞；不得覆盖仍被模块绑定的实例。 */
void hmi_diagnostics_init(hmi_diagnostics_t *diag);
/** @brief 更新单调时间；通过无符号差计算 age，要求两次观察间隔小于 2^32 ms。 */
void hmi_diagnostics_time(hmi_diagnostics_t *diag, uint32_t now);
/** @brief 拷贝一致的状态；调用者必须与全部写者串行化，函数不持内部 OS 锁。 */
bool hmi_diagnostics_snapshot(const hmi_diagnostics_t *diag, uint32_t now, hmi_diag_snapshot_t *out);
/** @brief 按 canonical key 查询 Domain 公共快照，未找到返回 false；无控制副作用。 */
bool hmi_diagnostics_signal(const hmi_diagnostics_t *diag, const char *key, uint32_t now,
                            hmi_diag_signal_t *out);
/** @brief 饱和递增，长期运行不回绕；只由所属 owner 调用。 */
static inline void hmi_diag_increment(uint32_t *counter)
{
    if (*counter < UINT32_MAX)
        ++*counter;
}
/** @brief 可选观测点；未绑定诊断实例时不做任何操作。 */
#define HMI_DIAG_INC(d, group, field)                                                                        \
    do                                                                                                       \
    {                                                                                                        \
        if (d)                                                                                               \
            hmi_diag_increment(&(d)->data.group.field);                                                      \
    } while (0)
/** @brief 结构化事件入口，无字符串格式化或输出。 */
#define HMI_DIAG_TRACE(d, module, event, now, a, b)                                                          \
    do                                                                                                       \
    {                                                                                                        \
        if (d)                                                                                               \
            hmi_trace_append(&(d)->trace, now, module, event, a, b);                                         \
    } while (0)
/** @brief CAN 观测类型；TX_BUSY 为端口暂未接受，不等同于硬件错误。 */
typedef enum
{
    HMI_CAN_RX,
    HMI_CAN_TX,
    HMI_CAN_RX_DROP,
    HMI_CAN_TX_BUSY,
    HMI_CAN_RX_ERROR,
    HMI_CAN_TX_ERROR
} hmi_diag_can_event_t;
/** @brief 在驱动/队列边界记录总线统计，正常 RX/TX 只累计 Counter。 */
void hmi_diagnostics_can(hmi_diagnostics_t *diag, hmi_bus_role_t bus, hmi_diag_can_event_t event,
                         uint32_t now);
#endif
