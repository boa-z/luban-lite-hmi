#ifndef HMI_CORE_H
#define HMI_CORE_H
#include "contracts/hmi_domain.h"
#include "diagnostics/hmi_diagnostics.h"
/**
 * @brief 域内核：只读快照加上它所绑定的产品存储。
 *
 * 存储数组属于调用方，必须比 core 活得更久；core 只做绑定与容量校验，不分配内存。
 * 复制该结构体会让两个副本共享同一组数组，嵌入式代码不应依赖这种别名行为。
 */
typedef struct hmi_core
{
    hmi_snapshot_t snapshot;
    hmi_diagnostics_t *diag;
    hmi_core_storage_t storage;
} hmi_core_t;
/**
 * @brief 绑定目录与存储，并把信号复位为 UNKNOWN、参数复位为 initial、故障复位为未激活。
 *
 * 目录不合法（身份为零或重复、监视项无法解析、参数范围异常）或任一容量不足时返回 false，
 * 此时既不写 core 也不写 storage。仅在启动阶段调用一次，不阻塞、不持锁。
 */
bool hmi_core_init(hmi_core_t *core, const hmi_catalog_t *catalog, const hmi_core_storage_t *storage);
/** @brief 绑定目录与存储后复制 Product 已确认的启动设置；NULL 保留兼容默认。 */
bool hmi_core_init_with_settings(hmi_core_t *core, const hmi_catalog_t *catalog,
                                 const hmi_core_storage_t *storage, const hmi_initial_settings_t *settings);
/**
 * @brief 写入一个信号值，作为 hmi_update_sink_t 使用。
 *
 * 身份未声明返回 false；非有限浮点降级为 ERROR。只在运行时线程调用（主机上与 UI 串行同线程）。
 */
bool hmi_core_apply(void *context, const hmi_update_t *update);
/** @brief 按目录中每个信号自己的 stale_ms 降级；0 表示不自动超时。 */
void hmi_core_tick(hmi_core_t *core, uint32_t now_ms);
/** @brief 更新连接状态与代数；断开时立即把所有 VALID 降级为 STALE，并保留最后可读值。 */
void hmi_core_connection(hmi_core_t *core, bool connected, uint32_t generation);
/** @brief 应用 UI 设置动作；越界或未知类别返回 false 且不改变状态。只在运行时线程调用。 */
bool hmi_core_action(hmi_core_t *core, const hmi_action_t *action);
/** @brief 只校验不写入：按身份检查参数值域，单位与范围由产品目录定义。 */
bool hmi_core_parameter_valid(const hmi_core_t *core, uint16_t id, float value);
/** @brief 校验通过后写入参数；失败时不改变现值。 */
bool hmi_core_parameter(hmi_core_t *core, uint16_t id, float value);
/** @brief 绑定诊断及公共 Domain 视图；目录/存储/诊断实例须覆盖绑定期，owner 线程调用。 */
void hmi_core_bind_diagnostics(hmi_core_t *core, hmi_diagnostics_t *diag);
#endif
