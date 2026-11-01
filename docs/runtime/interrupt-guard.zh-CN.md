# 关中断保护

> [English](interrupt-guard.md)

## 为什么有它

同一份固件在 D21x 板上，主界面待机时出现过两次崩溃。两次都在 LVGL 线程，栈余量充足，并且都指向
SDK `rt_hw_interrupt_disable` 的第一条指令：`0x4000033e` 处的 `csrrci a0, mstatus, 8`。

- `mcause=2`（非法指令），`mepc=0x4000033e`。这条指令在 M 模式下是合法的。
- `mcause=5`（load access fault），出错地址是合法的 `.bss` 地址。同一条 `csrrci` 没有写 `a0`，
  `rt_sem_take` 把信号量指针当成 `level`，`rt_hw_interrupt_enable` 又把它写进 `mstatus`。保存下来的
  `mstatus` 恰好等于该指针经过 WARL 合法化后的值，紧接着的数据读取就出错。出错原因是 `MPRV` 配合非法
  `MPP`，这一点是推断。

源码只有两条指令且正确。两次崩溃共同的是位置：这条 4 字节指令跟在 `.pendsv_exit` 后面落在 2 字节边界，
横跨 `0x40000340` 的 64 字节 cache line，是镜像里唯一这样的 CSR 指令。跨行是否就是原因尚未证实，
供电或时序余量问题同样符合现象。

## 它做了什么

`AIC_HMI_IRQ_GUARD`（默认 `y`）打开时，应用链接 `-Wl,--wrap=rt_hw_interrupt_disable` 和
`platform/rtthread/hmi_irq_guard.S`。对 SDK 函数的调用被解析到 `__wrap_rt_hw_interrupt_disable`，
它是同样两条指令的拷贝，从 64 字节边界开始。SDK 源码和 `rt_hw_interrupt_enable` 都不变。

## 在板子上对比两种布局

`AIC_HMI_IRQ_STRESS`（默认 `y`，约 1 KB）增加一条 shell 命令：

```
hmi_irq_stress <real|aligned> [seconds]
```

`real` 跑链接出来的 SDK 那份，`aligned` 跑保护用的那份。镜像里其余调用点都用保护那份，所以 `real` 只让压力循环
暴露在 SDK 布局下。每次调用前先给 `a0` 预置一个标记，再检查返回值。调用在一个 tick 内连续进行，然后命令睡 1 ms。
命令运行期间会把自己的优先级降到看门狗监督线程之下，这样跑飞的循环不会饿死喂狗，也就不会被误认为 CPU 卡死。
请在界面运行时测试，让显示中断保持活跃。结果行每 2 秒打印一次，给出迭代数和下列计数：

| 计数 | 含义 |
|---|---|
| `unwritten` | `a0` 仍是标记：`csrrci` 没有写回 |
| `no_mie` | 返回的 level 里 `MIE` 为 0 |
| `lost_mie` | `rt_hw_interrupt_enable` 之后 `MIE` 仍为 0 |
| `tick_stalls` | 约 0.8 s tick 没有推进，同时打印 `mstatus`、`mie`、`mip` |
| `mhz` | 用 `mcycle` 对照 4 MHz 计时器得到的核心主频，只在 burst 期间统计 |
| `per_ms` | 每毫秒 burst 时间内的迭代数 |
| `irq` | 这个周期里次数增加最多的两个中断源，格式为 `#编号+增量` |

`per_ms` 下降由另外两个数解释。`mhz` 减半说明核心时钟变了；`mhz` 不变而 `per_ms` 减半，说明 CPU 被中断或其他线程占走，
`irq` 给出最忙的中断源。`list_irq` 把编号对应到处理函数，GPIO 引脚的编号排在最后一个外设中断之后。

非法指令陷入无法计数，shell 会在陷入处理里打印最近一次的计数，记下那一行的 `iterations` 和 `elapsed_ms`。

没有陷入打印的复位，从启动日志判读：`Reset flag: 0x400` 且 `Watchdog-Reset` 表示硬件看门狗超时，bit 0 是上电，
bit 8 是外部引脚。压力命令在监督线程之下运行时，看门狗复位说明 CPU 或中断通路停了，而不是监督线程被饿死。

| 观察 | 判断 |
|---|---|
| 相近时长内 `real` 出问题而 `aligned` 没有 | 布局是触发条件，保留保护 |
| 两者都出问题 | 不是布局；把计数作为证据去查硬件余量 |
| 两者都没出问题 | 无结论；原来的故障出现在长时间待机之后 |

发布版本请关闭 `AIC_HMI_IRQ_STRESS`。原因未定之前保持 `AIC_HMI_IRQ_GUARD` 打开。

## 卡死哨兵

看门狗复位只留下 `Reset flag`，看不出是谁占着 CPU。打开 `AIC_HMI_LOCKUP_SENTINEL`（默认 `y`）后，心跳线程
`hmi_hb` 以监督线程的优先级运行，tick 中断里的钩子盯着它。启动 20 秒之后，心跳停了 3 秒（早于 8 秒的硬件超时）
就开始打印，最多 6 条，每秒一条：

```
lockup sentinel: heartbeat starved 3000 ms, running=hmi_nvm prio=21 mepc=40012345 mstatus=a00007880
  irq since snapshot (850 ms ago): #7 +850, #41 +12 (list_irq shows the numbers)
```

`running` 是占着 CPU 的线程，`mepc` 是被打断的指令，用 `riscv64-unknown-elf-addr2line -f -e d21x.elf <mepc>` 解析。
出现 `heartbeat resumed` 说明停滞是暂时的。看门狗复位之前完全没有输出，说明中断也停了：要么硬件卡死，要么有代码在
`MIE` 清零的状态下空转。
