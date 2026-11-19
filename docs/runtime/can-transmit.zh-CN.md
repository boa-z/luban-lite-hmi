# RT-Thread 与 ArtInChip 的 CAN 发送

[English](can-transmit.md)

## 驱动契约

以下结论来自当前集成使用的 SDK。升级 SDK 后应重新核对，不把它们当作所有 RT-Thread 版本的保证。

| 源码 | 实际行为 |
|---|---|
| `kernel/rt-thread/components/drivers/can/can.c` 的 `_can_int_tx` | INT_TX 获取原生邮箱信号量、调用 sendmsg、等待 completion；只有 RT_CAN_SND_RESULT_OK 才返回字节数。单帧拒绝、TX_FAIL 或完成超时返回 0。 |
| `rt_hw_can_isr` 与 `bsp/artinchip/drv/can/drv_can.c` 的 `aic_can_callback` | TX_DONE/TX_FAIL 更新原生结果并唤醒 completion。AIC 实际使用一个硬件发送缓冲，不使用传入的邮箱编号。 |
| `kernel/rt-thread/include/rtdef.h` | RT_WAITING_TIMEOUT 为 1000 tick。CAN 配置 ticks=50 控制状态定时器，不控制这个等待。 |
| `bsp/artinchip/hal/can/aic_hal_can.c` 的 `hal_can_send_frame` | 硬件缓冲忙时拒绝写入。正常模式使用控制器重传；completion 等待超时不会撤销已提交硬件的帧。 |
| `hal_can_error_handle` 与 drv_can.c 初始化 | AIC 的 BUS_OFF 状态值为 4；HAL 报 TX_FAIL，默认启用 autoBusoff 时释放复位，恢复时由错误警告中断更新状态。持续物理故障不保证恢复。 |

完整写入长度是当前可用的完成成功依据。返回 0 加上非 BUS_OFF 状态不能证明发送成功；失败也不能证明帧从未上线，因此 Framework 不自动重放。修改波特率会进入/退出硬件复位，只由卡死看门狗在连续失败 2 s 后使用一次，不作为逐帧重试。

原生机制的全部动作是：错误中断里按 `CAN_STAT` 更新状态，BUS_OFF 时回调 TX_FAIL 并清 `MODE.RST` 一位。它不重写模式位、不刷新软件状态、没有超时。总线持续异常，或热复位（外设域不复位）后残留 `SLEEP`/`LISTEN` 时，控制器会一直不可写且不再产生中断，Framework 必须自己兜底。

`RT_CAN_CMD_GET_STATUS` 返回由错误中断更新的软件缓存，硬件恢复后仍可能读到旧的 BUS_OFF。ArtInChip CAN v1 端口每次提交前读取 `CAN_STAT.BUS`。HAL 发出 BUS_OFF 的 `TX_FAIL` 回调或启动自动恢复前，也按这个硬件位判断。

## Framework 策略

每条已声明总线仍只有原来的 TX worker，串行执行发送、配置与永久停止，没有新增线程或重试队列。

- ArtInChip CAN0 的 HAL 在首次设速率前保持复位。打开 `can0` 时先设速率，再进入 NORMAL 并开启中断。其余设备继续先进入 NORMAL 再设速率。
- 提交前查询状态，BUS_OFF 或状态不可用时不再次尝试硬件写，只累计卡死时间。连续失败达到 2 s 时看门狗复位一次控制器（`SET_MODE(NORMAL)` + `SET_BAUD`，重写位时序并退出复位），最小间隔 2 s；任何一次成功发送都清零。复位只发生在这条总线的 TX worker 里，与发送、改速率天然串行，不按错误次数逐帧重试，也不自动重发失败帧。
- ArtInChip CAN v1 在硬件 BUS_OFF 位清除后恢复提交，该位有效期间抑制写入。未映射硬件状态的端口仍使用驱动状态，复位或改速率后放行一次探测写。
- 完整写计成功；零/短写、状态不可用和 BUS_OFF 计失败。失败后立即服务下一队列项，不休眠 100～1000 ms，也不自动重发。普通帧失败后进入每条总线的退避窗口（50 ms 起，连续失败翻倍，上限 200 ms），窗口内普通帧不再调用驱动；显式优先帧仍立即尝试。下一次成功发送清除窗口。这样硬件发送缓冲卡住时，高优先级 TX 线程不会反复走 SDK HAL 的拒绝日志而挤占低优先级界面线程。每条总线的 Framework 错误摘要首次立即记录，此后每秒最多一次，计数保留每次失败。HAL/驱动诊断沿用 SDK 自身日志策略。
- 显式协议优先帧先于普通积压发送。运行时优先级 17 高于 TX 的 18，积压不能阻止协议调度；
- 驱动提交前检查期限、会话及可选协议事务约束；事务检查后再次复核期限、会话和停止状态。过期或撤销帧直接丢弃，不调用驱动。超时、取消与 Abort 属于协议层，worker 不推进 SDO 状态机。
- 停止请求拒绝新提交并丢弃排队帧，当前驱动调用返回后用原生 LISTEN 模式结束总线参与，直到重启。

## 时序与验证

新入队的优先帧前不再叠加失败休眠；普通帧退避只丢弃帧，不阻塞 worker，也不推迟优先提交。周期调度保持相位并跳过错过的周期。已提交硬件的写不能抢占或撤回；一次驱动调用仍可能等待 1000 tick（1 kHz 配置下为 1000 ms），因此缺失完成中断或总线故障时的严格线上周期保证超出本轮 Framework 修改。控制器完全不动时（残留 SLEEP/LISTEN，或总线被持续拉显性），第一帧会被硬件接受但 completion 等满 1000 tick，之后硬件发送缓冲一直忙，看门狗在 2 s 后才会复位——这也是区分“无 ACK”（立即报错）与“根本没在发”的依据。正常总线周期与恢复必须通过板端带时间戳 CAN 抓包验证；主机周期测试只证明调度和队列策略。

can-execution 执行真实 worker，控制队列与硬件替身：普通及 checked 发送失败、积压时优先发送、普通帧退避与清除、连续错误日志汇总、事务检查期间到期、取消、会话切换、配置和停止。can-port 执行真实端口：完整/零/短写、状态失败、BUS_OFF 抑制、打开时强制 NORMAL、连续失败 2 s 后复位一次与最小间隔、复位后探测写（成功放行、失败收回、只放行一次）、成功清零、warning/passive、修改速率和 LISTEN。测试不模拟 completion IRQ 时序，不证明电气恢复。
