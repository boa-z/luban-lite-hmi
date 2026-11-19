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

完整写入长度是当前可用的完成成功依据。写入失败时，帧是否上线仍未确定。Framework 继续处理下一条队列记录，不重发该帧。

底层在 BUS_OFF 时回调 TX_FAIL 并清除 `MODE.RST`。控制器应处于 NORMAL、但模式位仍异常时，Framework 可以恢复模式和位时序。

`RT_CAN_CMD_GET_STATUS` 返回由错误中断更新的软件缓存，硬件恢复后仍可能读到旧的 BUS_OFF。ArtInChip CAN v1 端口每次提交前读取 `CAN_STAT.BUS`。HAL 发出 BUS_OFF 的 `TX_FAIL` 回调或启动自动恢复前，也按这个硬件位判断。

## Framework 策略

每条已声明总线仍只有原来的 TX worker，串行执行发送、配置与永久停止，没有新增线程或重试队列。

- ArtInChip CAN0 的 HAL 在首次设速率前保持复位。打开 `can0` 时先设速率，再进入 NORMAL 并开启中断。其余设备继续先进入 NORMAL 再设速率。
- ArtInChip CAN v1 仅在硬件已退出 BUS_OFF 且发送缓冲可写时提交，底层处理 BUS_OFF 自动恢复。发送失败且持续观测到异常模式位至少 2 s 时，Framework 才重写模式和位时序。未映射硬件寄存器时，以驱动报告的 BUS_OFF 作为恢复依据，复位或改速率后放行一次探测写。发送成功或异常指示消失时清零。TX worker 串行执行恢复、发送和改速率。
- 完整写计成功；零/短写、状态不可用和 BUS_OFF 计失败。失败后立即服务下一队列项，不休眠 100～1000 ms，也不自动重发。普通帧失败后进入每条总线的退避窗口（50 ms 起，连续失败翻倍，上限 200 ms），窗口内普通帧不再调用驱动；显式优先帧仍立即尝试。下一次成功发送清除窗口。这样硬件发送缓冲卡住时，高优先级 TX 线程不会反复走 SDK HAL 的拒绝日志而挤占低优先级界面线程。每条总线的 Framework 错误摘要首次立即记录，此后每秒最多一次，计数保留每次失败。HAL/驱动诊断沿用 SDK 自身日志策略。
- 显式协议优先帧先于普通积压发送。运行时优先级 17 高于 TX 的 18，积压不能阻止协议调度；
- 驱动提交前检查期限、会话及可选协议事务约束；事务检查后再次复核期限、会话和停止状态。过期或撤销帧直接丢弃，不调用驱动。超时、取消与 Abort 属于协议层，worker 不推进 SDO 状态机。
- 停止请求拒绝新提交并丢弃排队帧，当前驱动调用返回后用原生 LISTEN 模式结束总线参与，直到重启。

## 时序与验证

TX worker 连续服务队列达 5 ms 后休眠 1 ms，空队列时等待新任务。已经提交给硬件的发送无法抢占或撤回，等待发送完成仍可长达 1000 tick（1 kHz 配置下为 1000 ms）。用 `hmi_exec` 对照每路板端发送调用次数、返回次数和最大耗时。正常总线周期与恢复需通过板端带时间戳的 CAN 抓包验证。主机测试覆盖调度和队列策略。

`can-execution` 覆盖队列策略、接收时间和帧数预算、发送调用与返回计数、事务、配置和停止。`can-port` 覆盖驱动结果、BUS_OFF 回退恢复及最小间隔、缺乏故障证据时抑制复位、单次探测写、改速率和 LISTEN。完成中断时序和电气恢复需实板验证。
