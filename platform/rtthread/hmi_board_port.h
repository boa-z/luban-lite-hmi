#ifndef HMI_BOARD_PORT_H
#define HMI_BOARD_PORT_H
#include "contracts/hmi_can_frame.h"
#include "diagnostics/hmi_diagnostics.h"
/* 板级适配：设备名与显示层策略来自 Kconfig（Board resources），不在这里写死某块板。 */
struct rt_semaphore;
/** @brief 读取 SDK 单调硬件毫秒时钟；32 位差值允许正常回绕，不依赖 RTOS tick 初值。 */
uint32_t hmi_board_now_ms(void);
/** @brief LVGL 线程在 SDK 建好显示与触摸之后调用：按 Kconfig 接管显示层并加载色彩校正。 */
bool hmi_board_display_init(void);
/** @brief LVGL 线程清除显示诊断标志；显示本身属于 SDK，仅启动失败路径使用。 */
void hmi_board_display_close(hmi_diagnostics_t *diag);
/** @brief LVGL 线程复制显示刷新次数与 LVGL 可见的触摸状态；无额外采样或串口输出。 */
void hmi_board_diagnostics(hmi_diagnostics_t *diag);
/** @brief 绑定 RX 唤醒信号量；任一已打开总线收到帧时释放。 */
void hmi_board_can_wake(struct rt_semaphore *wake);
/** @brief 运行时线程按 Kconfig 映射打开逻辑总线并设定速率。 */
bool hmi_board_can_open(hmi_bus_role_t bus, uint32_t bitrate, hmi_diagnostics_t *diag);
/** @brief 运行时线程轮询各已打开总线的接收 FIFO，取一帧。 */
bool hmi_board_can_read(hmi_can_frame_t *frame);
/** @brief 该总线的发送线程调用；驱动可能等待硬件。 */
bool hmi_board_can_send(const hmi_can_frame_t *frame);
/** @brief 该总线发送 worker 在当前驱动写返回后调用：转入原生只听模式，不发送或 ACK。 */
bool hmi_board_can_stop(hmi_bus_role_t bus);
/** @brief 唯一 TX worker 修改已打开总线的速率，与发送和下线串行执行。 */
bool hmi_board_can_configure(hmi_bus_role_t bus, uint32_t bitrate);
/** @brief 复制原生 CAN 驱动的丢包与错误计数。 */
void hmi_board_can_diagnostics(hmi_diagnostics_t *diag);
#endif
