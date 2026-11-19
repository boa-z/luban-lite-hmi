# 看门狗监督

> [English](watchdog.md)

## 归属

`runtime/hmi_watchdog.c` 只拥有判据，别的都不做：它接收每个 owner 的心跳计数，加上"此刻应当
推进"的通道集合，回答是否可以喂硬件看门狗。它不打开设备、不创建线程、不持有锁，因此主机测试
可以覆盖每个分支。`platform/rtthread/hmi_watchdog_port.c` 拥有板级那一半：`"wdt"` 设备、
一个最低优先级的线程、keepalive 调用和 `hmi_watchdog` 串口命令。

监督名单就是应用要如实工作就必须持续运行的两个常驻线程：运行时线程（`app`）与 UI。计数来自
`hmi_execution_liveness()`，它读取两者的循环计数。看门狗从不检查其它 owner 的内部。

## 何时进入监督

| 执行状态 | 是否监督 | 硬件看门狗 |
|---|---|---|
| runtime 尚未启动（启动阶段） | 否 | 未启动 |
| 运行时已启动 | app | 已启动 |
| 首次呈现成功且 Product 启用 UI 监督 | app 和 UI | 已启动 |
| 启动失败 | 否 | 未启动 |

监督线程第一次观察到 runtime 已启动时才启动硬件看门狗。在此之前端口只轮询，因此缓慢的启动
（例如较长的 EEPROM 初始化）不可能被复位。运行时没有停止流程，因此一旦启动就一直保持到复位。

因此启动阶段本身不在监督窗口内。runtime 启动之前的隐藏致命故障不会被这套机制转成复位，
而是由启动路径和 `hmi` 诊断报告。覆盖启动阶段需要一个由入口点喂的启动进度通道，
这部分刻意不属于本契约。

## 阈值

每个通道有自己的停滞上限，所以允许某个 owner 偏慢而不必放松其它通道。通道只在被期望时才被
判定；离开期望集合时基线被丢弃，重新进入总是重新起算，因此模式切换不会制造瞬间的误判停滞。

| 预算 | 默认值 | 含义 |
|---|---|---|
| `HMI_WATCHDOG_TIMEOUT_S` | 8 | 硬件超时，单位秒 |
| `HMI_WATCHDOG_POLL_MS` | 200 | 监督采样周期 |
| `HMI_WATCHDOG_APP_STALL_MS` | 4000 | 运行时线程心跳上限 |
| `HMI_WATCHDOG_UI_STALL_MS` | 3000 | UI 心跳上限 |
| `HMI_WATCHDOG_THREAD_PRIO` | 26 | 监督线程优先级，低于所有被监督 owner |

它们全部位于 `platform/rtthread/hmi_watchdog_budget.h` 且可被 `#ifndef` 覆盖，因此换板时
只需重设预算而不碰判据。启用开关是 Kconfig 选项 `AIC_HMI_WATCHDOG`，默认打开；
关掉后端口只剩一个记录用的空实现。没有 `"wdt"` 设备的板子只记录一次并保持不监督，
而不是启动失败。

监督线程的优先级低于所有被监督 owner，因此只在其它 owner 让出 CPU 时才被调度。
更高优先级的忙等会饿死监督线程并随后触发硬件复位——这是设计意图，不是副作用。

## 诊断

当前单核目标用 RT-Thread 调度临界区保护状态推进和报告复制。诊断直接读取权威状态，不维护重复计数。
设备 I/O 与日志均在临界区外。报告接口供线程调用，包括 MSH。

```
hmi_watchdog
watchdog armed=1 supervising=1 timeout_s=8
watchdog feeds=812 withheld=0 stalls=0 transitions=1 stalled=2
watchdog channel=app stall_ms=4000
watchdog channel=ui stall_ms=3000
```

`withheld` 统计被拒绝喂狗的采样数，`stalls` 统计进入停滞态的次数；`stalled` 是第一个停滞通道的
下标，健康时为通道总数。`withheld` 非零但没有复位，说明看门狗在硬件超时之前恢复了，
这正是 3-4 秒停滞上限与 8 秒硬件超时之间预留的余量。

## 限制

停滞上限是工程预算，不是实测最坏值。在做出安全相关声明之前，仍需目标板上的最慢 owner 周期、
最长合法阻塞区间与故意故障注入证据。寄存器写保护（`RT_DEVICE_CTRL_WDT_EN_REG`）刻意未启用，
因为本框架还没有硬件证据表明启用后 keepalive 仍然有效。

owner 清单及其优先级见[运行时生产架构](runtime-production.md)。
