#ifndef HMI_NVM_PORT_H
#define HMI_NVM_PORT_H
#include "core/hmi_settings.h"
#include "diagnostics/hmi_diagnostics.h"
#include "storage/hmi_nvm.h"
/* 除 worker 线程外，下列函数都只由运行时线程调用；介质 I/O 全在 worker 里完成。 */
/** @brief 按 Product 存储策略启动 NVM worker 并投递首次读取；调用前 Core 已初始化。 */
bool hmi_board_nvm_start(hmi_core_t *core);
/** @brief 领取 worker 结果、处理 MSH 维护请求并调度保存；不做阻塞 I/O。 */
void hmi_board_nvm_poll(uint32_t now, hmi_diag_storage_t *status);
/** @brief 设置可能变化后调用：编码当前设置，内容不同才进入去抖保存。 */
bool hmi_board_nvm_changed(uint32_t now);
/** @brief 用户动作已生效：设置内容变了就申请立即保存，不等去抖窗口。
 *  只用于已确认的离散动作（改密码、改选项等）；app_tick 产生的周期变化仍走去抖。 */
bool hmi_board_nvm_action_applied(uint32_t now);
/** @brief 启动读取是否已结束（未启用持久化视为已结束），避免迟到的读取覆盖用户修改。 */
bool hmi_board_nvm_ready(void);
/** @brief 升级激活前申请立即保存，返回需要等到 durable 的版本号；0 表示无需等待。 */
uint64_t hmi_board_nvm_flush(void);
/** @brief 目标版本是否已经写入介质。 */
bool hmi_board_nvm_barrier(uint64_t target);
#endif
