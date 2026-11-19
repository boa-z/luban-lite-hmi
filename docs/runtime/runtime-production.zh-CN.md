# 生产运行时

> [English](runtime-production.md)

## 线程

一个运行时线程独占 Domain（Core）、协议适配器和全部 Product 回调。只有可能阻塞在硬件上的调用才放在单独的线程里。

| 线程 | 拥有 / 负责 | 唤醒条件 | 栈 |
|---|---|---|---|
| `hmi_app`，优先级 17 | Core、协议路由与适配器、Product 的 `app_run`/`app_tick`/`local_action`、周期帧调度、快照发布 | CAN 接收、UI 动作，或 5 ms | 12288 字节 |
| `hmi_tx0` / `hmi_tx1`，优先级 18 | 每条已打开的总线一个；唯一调用 CAN 驱动写入的线程 | 自己的队列 | 各 2048 字节 |
| `hmi_ui`，LVGL 优先级 + 2 | LVGL、Product UI | 16 ms | 默认 32768 字节 |
| `hmi_wdog`，优先级 26 | 喂硬件看门狗 | 200 ms | 2048 字节 |

除 UI 工厂和发送前 valid 检查外，所有 Product 回调都在运行时线程里依次执行；UI 提交与 TX 状态查询仍须由 Product 保护共享状态。主机模拟器用 `hmi_runtime_step()` 在一个循环里跑同样的顺序。

## 运行时线程的一轮

2. 从已打开的总线读至多 64 帧。每帧立即路由并解码，解码直接写入 Core。
3. 应用排队的 UI/MSH 动作（`HMI_ACTION_PRODUCT` 交给 `local_action`，其余交给 Core）。
4. `hmi_runtime_step()`：适配器 `process`、带命令端口的 Product `app_run`、信号超时、Product `app_tick`。
5. 用当前快照编码到期的周期帧并入队。
6. 把亮度设置写到背光。
7. 发布：快照 revision 变化时复制给 UI，并复制诊断数据给 MSH。

接收切片在处理 64 帧或观测到接收耗时达到 2 ms 时结束。Runtime 随后用最新时间推进产品业务和周期发送，若达到接收预算或已连续活动 5 ms，则休眠 1 ms，为 UI 和看门狗留出调度机会。CAN 驱动状态在启动时采样，之后最多每 100 ms 采样一次。看门狗统计完整结束的 Runtime 循环。

## 线程之间的接口

| 从 → 到 | 机制 | 深度 |
|---|---|---|
| UI / MSH → 运行时 | `hmi_execution_action()`；只有队列满或当前不接收设置时返回 false | 8 个动作 |
| 运行时 → UI | `hmi_execution_present()`；revision 变化时 UI 才在发布锁内复制 | 一份发布副本 |
| 运行时 → 发送 | 普通队列与优先队列 | 每条总线 16 + 16 |

发布锁（`hmi_debug_lock`）同时保护公共诊断，所以 `hmi domain` 与 `hmi signal` 读到的和 UI 是同一份副本。

## 受约束的发送与设备配置

send_checked 把可选优先级、期限、事务 tag 和只读 valid 回调复制入现有队列。valid 在 TX worker 交给驱动前执行，只能查询协议状态，不得执行 I/O、修改协议栈或重新发送；context 必须存活到排队帧不再引用它。会话切换、期限、事务撤销和永久下线均可使帧作废。所有发送失败后均继续服务下一队列项，不让整个 worker 休眠；普通帧失败后进入每条总线 50–200 ms 的退避窗口，窗口内普通帧不调用驱动，显式优先帧仍立即尝试。运行时优先级 17 高于 TX 的 18，普通发送积压不能推迟协议调度入队；已经开始的硬件写不能抢占。

只有 RT-Thread 返回完整写入长度才计为 TX 成功。TX worker 连续服务队列达 5 ms 后休眠 1 ms，空队列时等待新任务。普通帧退避只丢帧。控制器异常状态需连续观测至少 2 s 才尝试复位，两次尝试至少相隔 2 s。每条总线的错误摘要每秒最多一次，事务检查后再次复核期限。完成、恢复和时序边界见 [CAN 发送行为](can-transmit.zh-CN.md)。

configure 由运行时反复查询同一请求，唯一 TX worker 串行执行速率修改并发布成功/失败；运行时不得等待设备锁。已完成但未领取的旧结果可由不同速率的新请求替换；正在执行的请求不能替换。Product 负责保存与后续设备通知。stop 拒绝后续排队并丢弃旧帧，当前写返回后使用原生控制器接口下线，重启前不恢复。

## 命令

`hmi_command_port_t.send()` 按 Product 的 `command_route` 把命令交给所属适配器的 `command()`，在同一次调用里返回适配器是否接受。需要等待应答的事务（SDO、参数读写、版本查询）由适配器自己负责：在 `process` 里发帧、在 `on_frame` 里匹配应答、自己处理超时，结果以信号发布。

## 周期帧

`hmi_periodic_frame_t` 给出总线、标识、长度和周期；期限到达时 `encode(snapshot, now, frame)` 用当前快照填写数据。是否新鲜由编码器按各信号的状态自己判断。每个条目最多一帧在发送队列里等待；一个周期内还没交给驱动的帧被丢弃，所以总线无应答时不会积压。迟到的期限跳过错过的周期，不补发。

## 会话

运行时启动会建立新的会话代次、使旧信号过期、重置适配器并调用 Product 的 `app_reset`。发送队列保留代次检查。

## 启动

RT-Thread 主线程组装 Product、调用准备与启动回调，然后启动运行时。运行时应用开机设置，打开 Product 所需总线并发布首份快照。SDK 的 LVGL 定时器在首份快照可用后接入界面。支持无界面的 Product 可在关闭 LVGL 的固件中启动运行时。界面初始化失败时记录日志，运行时继续执行。运行时成功启动后，看门狗开始监督应用。Product 可选择在首次成功呈现后监督界面。

Product 负责自己设置的持久化。可选的 `runtime_init` 在打开总线前恢复启动所需状态。已执行的本机 Action 可用 `action_applied` 触发 Product 自己的保存。

## 诊断

`hmi_exec` 打印 Runtime 已完成轮次、接收预算命中次数及接收切片最大耗时、发送队列满次数、每条总线的发送结果、板端发送调用次数及返回次数、板端发送最大耗时，以及周期条目错过次数和最大迟到。发送调用次数和返回次数持续相差，表示有发送调用尚未返回。其他命令见[诊断](diagnostics.zh-CN.md)。

## 构建期 Product 选择

每个固件镜像只包含一个 Product，SCons 按 defconfig 的 `CONFIG_AIC_HMI_PRODUCT_ROOT` 选择；环境变量 `HMI_PRODUCT_ROOT` 可在单次构建中覆盖。选择为空时固件构建停止。主机 CMake 在 `-DHMI_PRODUCT_ROOT` 指定 Product 之前只构建框架。所选 `product/sources.json` 提供固件组装（`contracts/hmi_firmware.h`）：三份互不重叠的存储（运行时 Domain、发布副本、UI 副本）以及语言初始化。
