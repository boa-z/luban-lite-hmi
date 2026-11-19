#ifndef HMI_EXECUTION_PORT_H
#define HMI_EXECUTION_PORT_H
#include "contracts/hmi_product.h"
#include "core/hmi_core.h"
/**
 * @brief 固件运行时：一个运行时线程独占 Core、协议适配器与 Product 回调，每条已打开的 CAN
 * 总线一个发送线程（驱动发送可能等待硬件）。UI 线程只经动作队列提交意图，并在发布锁内复制快照。
 */
typedef struct
{
    const hmi_product_t *product;
    hmi_core_t *core;
    /** @brief 发布给 UI 的快照副本存储，不得与 Core 或 UI 自己的副本重叠。 */
    hmi_core_storage_t published_storage;
    /** @brief 公共诊断实例（MSH `hmi` 查询），由 hmi_debug_lock 保护。 */
    hmi_diagnostics_t *public_diagnostics;
} hmi_execution_config_t;
/** @brief UI 线程启动期调用一次：建立队列并启动运行时线程（设置加载、开总线都在该线程里完成）。 */
bool hmi_execution_start(const hmi_execution_config_t *config);
/** @brief UI/MSH 提交本机设置动作；true 只表示已入队，结果以之后发布的快照为准。 */
bool hmi_execution_action(void *context, const hmi_action_t *action);
/** @brief 运行时是否接收设置：已启动、设置已加载。 */
bool hmi_execution_settings_allowed(void);
/**
 * @brief UI 线程在发布锁内复制最新快照；快照未变化时不重复复制。
 * @return false 表示运行时尚未发布第一份快照，此时不得渲染。
 */
bool hmi_execution_present(hmi_snapshot_t *snapshot, const hmi_core_storage_t *storage);
/** @brief UI 线程每轮发布显示与触摸统计，同时作为看门狗的 UI 心跳。 */
void hmi_execution_ui_diagnostics(const hmi_diagnostics_t *diagnostics);
/** @brief 看门狗采样：运行时线程与 UI 线程的循环计数；运行时尚未启动时返回 false。 */
typedef struct
{
    uint32_t app_runs, ui_ticks;
} hmi_execution_liveness_t;
bool hmi_execution_liveness(hmi_execution_liveness_t *out);
#endif
