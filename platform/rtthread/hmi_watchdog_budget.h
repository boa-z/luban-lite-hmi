#ifndef HMI_WATCHDOG_BUDGET_H
#define HMI_WATCHDOG_BUDGET_H
/**
 * @brief 板级看门狗预算；可由构建定义覆盖，不改变 runtime/hmi_watchdog.c 的公共判据。
 *
 * 启用开关是 Kconfig 的 AIC_HMI_WATCHDOG（默认 y）。关掉它时整个端口不编译，
 * 只保留一个记录用的空实现，因此这里不再重复一个 enable 宏。
 */
/**
 * @brief 硬件超时（秒）。
 *
 * 监督线程每 HMI_WATCHDOG_POLL_MS 喂一次，因此超时必须远大于采样周期：
 * 默认 8 秒相当于允许连续丢失 40 次喂狗，正常调度抖动不会触发复位。
 */
#ifndef HMI_WATCHDOG_TIMEOUT_S
#define HMI_WATCHDOG_TIMEOUT_S 8
#endif
/** @brief 监督线程采样周期（毫秒）。 */
#ifndef HMI_WATCHDOG_POLL_MS
#define HMI_WATCHDOG_POLL_MS 200u
#endif
/** @brief 各 owner 允许的最长停滞时间；0 表示该通道只被观察、不参与判停滞。 */
#ifndef HMI_WATCHDOG_APP_STALL_MS
#define HMI_WATCHDOG_APP_STALL_MS 4000u
#endif
#ifndef HMI_WATCHDOG_UI_STALL_MS
#define HMI_WATCHDOG_UI_STALL_MS 3000u
#endif
/**
 * @brief 监督线程优先级：必须低于全部被监督的 owner（运行时=19、NVM=21、UI=22），
 * 这样它只在其余 owner 都让出 CPU 时才运行；任何高优先级忙等都会导致停止喂狗。
 */
#ifndef HMI_WATCHDOG_THREAD_PRIO
#define HMI_WATCHDOG_THREAD_PRIO 26
#endif
#ifndef HMI_WATCHDOG_THREAD_STACK
#define HMI_WATCHDOG_THREAD_STACK 2048u
#endif
#endif
