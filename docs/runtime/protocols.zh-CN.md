# 协议与 Domain 契约

## Domain 修订号

Core revision 比较完整的 `hmi_value_t`（时间戳、状态、来源和值）；参数、故障、连接、设置与设备型号配置变化也会推进它。UI 只在 revision 变化时复制发布的快照。信号的 `stale_ms` 为零表示不自动超时。来源仲裁拿到完整的当前值与新值。

## 协议服务边界

接收路由把（总线、标识、格式）映射到唯一 owner。owner 要么是无状态的 `hmi_decode_fn_t`（由 DBC 生成），要么是有状态的 `hmi_protocol_adapter_t`，带 `on_frame`、`process`、`command` 与 `reset`。`hmi_protocol_services_t` 携带 Domain 更新入口、CAN 发送端口与可选诊断，只在回调期间有效。解码在运行时线程里直接写入 Core。

信号是持续状态；命令是 App 发起的业务请求。`hmi_command_port_t.send()` 把命令交给 Product 的 `command_route` 选定的适配器，并返回适配器是否接受。请求/应答事务及其超时、重试都是适配器自己的状态；结果以信号送到 UI。运行时没有全局唯一的事务、没有请求 id，也没有自己的传输超时。

Core 不认识 CAN；UI 不认识 CAN；Runtime 没有 Product 分支。

## Demo 仪表发送 PDO

Demo Product 以 100 ms 发布两帧公开合成的 CAN0 标准数据帧，DLC 8，小端。只发送的定义是 `products/demo/protocol/can/demo_tx.dbc`。这些示例不实现 CANopen NMT、SYNC、心跳、可配置 PDO 映射或对象字典，也不是车辆控制命令。

| 帧 | 字节 / 位 | 含义 |
|---|---|---|
| 0x381 demo_tpdo_motion | 0–1 / 2–3 / 4–5 / 6 | 车速 0.01 km/h / 起升高度 0.001 m / 载重 kg / SOC 百分比 |
| 0x381 demo_tpdo_motion | 字节 7 位 0 / 位 1–7 | 整组新鲜 / 7 位序号 |
| 0x481 demo_tpdo_status | 字节 0 位 0–4 | 座椅有人 / 制动 / 空挡 / 充电 / 告警 |
| 0x481 demo_tpdo_status | 字节 1 位 0 / 字节 2 / 字节 3 | 整组新鲜 / 8 位序号 / 布局版本 1 |
| 0x481 demo_tpdo_status | 字节 4–7 | 运行时会话代次，不是固件版本或持久计数 |

只有全部来源信号都 VALID 且在量程内时该组才新鲜；否则数值发零、fresh=0，有效的零仍是 fresh=1。序号每编码一次加一。两帧都是普通帧。`demo-pdo` 检查黄金向量与新鲜度规则；`demo-pdo-dbc` 用 cantools 解码实际的 C 输出。主机通过不能证明板上已经发出。
