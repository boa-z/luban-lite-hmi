# 复用 LVGL 与 SDK 服务

> [English](reuse.md)

本次优化以现有 Luban-Lite SDK 1.3.2 与 Framework 为基线，复用固定版本的 LVGL、RT-Thread 和 ArtInChip 服务。产品功能与 UI 通过 Product 接入这一组合，不以另一套 SDK 的接口作为前置条件。只有成熟接口能保持现有契约时，替换才有收益。设备访问属于 platform/BSP，客户措辞、协议和策略属于 Product，contracts/Core 保持独立于 LVGL 与 RT-Thread。

## 已落实的精简

- 可视控件（仪表盘、指针、环形/弧形条、线性表、状态指示、数值标签、升级页）、它们的数值格式化/映射助手以及控件词汇（`meter.*` 状态词）移入唯一的使用者 Demo。框架是多 Product 的运行平台，不是仪表控件库：`ui/common/` 只保留翻译服务和显示测试卡，架构门禁拒绝其他内容。
- 带类型的整数占位符改用 libc `snprintf()` 和固定的 `PRId32`/`PRIu32` 格式串。翻译模板仍按数据解析，保留参数重排、花括号转义和有界输出。
- 看门狗诊断用 RT-Thread 调度临界区直接复制权威状态，删除五个未同步的计数副本；状态推进使用相同保护。当前单核临界区内只有有界计算/复制，日志与设备调用都在区外。

## 按源码决定保留的内容

下表中 SDK 路径相对 SDK 根目录，Framework 路径相对本仓库。

| 模块与源码 | 已复用的成熟接口 | 剩余代码保留理由 |
|---|---|---|
| 国际化：`ui/common/i18n/hmi_i18n_runtime.c`；`ui/common/i18n/lv_i18n` | lv_i18n 语言包、当前语言、复数规则 | LVGL 9.1 没有 `lv_translation`。查表由 lv_i18n 提供；标签绑定、语言变化事件、显式语言查询、Product 拥有的持久化语言 ID 与字体角色仍由薄适配层负责。 |
| 显示：`platform/rtthread/hmi_board_port.c`；SDK `bsp/artinchip/drv/display/` | `mpp_fb_ioctl()`、`lv_aic_init()` | 显示层归属、额外 UI 矩形、alpha 和色键属于应用组合；有摄像头的 Product 可关闭独占。无需重做 framebuffer/渲染驱动。 |
| 背光：`platform/rtthread/hmi_board_settings.c`；SDK `bsp/artinchip/drv/display/panel/panel_com.c` | `rt_pwm_set()`、`rt_pwm_enable()`、`PWM_CMD_GET` | 面板助手只按固定周期和编译期占空比使能，不提供持久化亮度、配置底座、读回与重试。保留现有原生 PWM 适配及板级 Kconfig 参数。 |
| CAN：`platform/rtthread/hmi_board_port.c`、`hmi_execution_port.c`；SDK `kernel/rt-thread/components/drivers/can/can.c` | `rt_device_read/write/control()`、RX 通知、CAN 状态 | 驱动 TX 会等待邮箱/完成事件。保留每总线 worker，避免一条总线阻塞 Core、UI 或另一条总线。会话过期也不是原始驱动队列提供的能力。 |
| IPC：`hmi_execution_port.c`；SDK `kernel/rt-thread/src/ipc.c` | 原生消息队列、信号量、事件、互斥锁 | 队列复制请求/结果，单一运行时 owner 修改 Core，发布快照在互斥锁下复制。引入事件总线或跨线程共享可变 Core 会增加复杂度。 |
| RTC：`platform/rtthread/hmi_rtc_port.c`、`runtime/hmi_wall_clock.c`；SDK RTC 驱动与 libc `ctime.c` | `time()`、`gmtime_r()`、`set_timestamp()` | UTC 绑定、无效时钟过滤和 UI owner 的读缓存仍有用途。纯 64 位日历换算避免时区依赖及平台有符号 32 位写入范围的限制。 |
| 看门狗：`runtime/hmi_watchdog.c`、`platform/rtthread/hmi_watchdog_port.c`；SDK `bsp/artinchip/drv/wdt/drv_wdt.c` | RT-Thread timeout/start/keepalive 控制 | 设备负责复位 CPU，不判断 runtime/UI 是否健康推进。保留 owner 监督与启动门禁；改为空闲钩子喂狗会掩盖 UI 停滞。 |

## HMI 框架复核（2026-10-28）

更名为 Luban-Lite HMI Framework 时，按本树实际使用的 SDK（Luban-Lite 1.3.2）逐项复核了各机制。没有任何一项被改写到 SDK 服务之上，结论如下：

| 机制 | SDK 中的候选 | 结论 |
|---|---|---|
| 运行时线程、动作队列、发布快照（`hmi_execution_port.c`） | RT-Thread 消息队列、信号量、互斥量 | 保留。它本身就由原生 IPC 搭成，只额外提供 Core 的单一归属与一次加锁复制，SDK 没有对应服务。没有新增事件总线。 |
| UI 启动与呈现循环 | SDK LVGL 线程里的 `lv_user_gui_init()`、`lv_timer` | 原样保留：UI 由 SDK 线程运行，框架只经 `--wrap` 接入并加一个 `lv_timer`。 |
| 每总线 CAN 发送 worker、周期帧 | RT-Thread CAN 设备 | CAN Product 保留；没有路由也没有周期帧的 Product 现在一个都不启动。驱动写会在邮箱上阻塞，总线卡住时不能拖住 Core 与 UI。 |
| 背光、显示色彩、RTC、看门狗、SHA-256 | PWM、DE ioctl、RTC 设备、WDT 设备、mbedTLS | 本来就是 SDK 驱动之上的薄适配（见下文）。 |
| 结构化 Trace 与诊断命令 | ulog、MSH | 保留：ulog 负责打印，Trace 保存有界的二进制历史供事后在命令行导出；命令本身是原生 MSH。 |

本次删除：只属于 Demo 的目录内容（`hmi_monitor_def_t`、`HMI_SOURCE_DEMO`）以及两个没有调用者的函数（`hmi_core_snapshot`、`hmi_board_update_started`）。没有新增：信号有效期或超时状态、通用 HAL、插件、运行期 Product 注册表。

## 当前 SDK 的复用边界与验收

以下取舍以当前 SDK 的实际源码为依据：

- `kernel/rt-thread/components/drivers/can/can.c` 会等待发送邮箱/完成事件；保留每总线 worker，并在实板测量两条总线。
- `bsp/artinchip/drv/rtc/drv_rtc.c` 负责 2020-05-20 纪元与 UTC 秒的转换，Framework 继续使用标准时间接口与未对时过滤，不重复实现硬件纪元适配。
- `panel_com.c` 的背光助手只提供固定初始化；现有 RT PWM 端口负责动态亮度及读回。设备、通道等板级事实由 Kconfig/BSP 提供。
- 看门狗仍需验证启动、喂狗、复位和时间预算。诊断快照保护以当前单核、仅线程调用为前提。
