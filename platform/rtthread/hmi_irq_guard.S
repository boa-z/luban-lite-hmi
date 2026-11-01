/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * D21x 的 rt_hw_interrupt_disable，由 -Wl,--wrap=rt_hw_interrupt_disable 链接进来替换 SDK 那份
 * （SConscript，AIC_HMI_IRQ_GUARD）。
 *
 * SDK 那份也是一条 csrrci，但它跟在 .pendsv_exit 后面落在 2 字节边界上：这条 4 字节指令横跨
 * 0x40000340 的 64 字节 cache line。这里让函数从 cache line 起点开始，两条指令落在同一行。
 * 行为不变：返回原 mstatus 并清 MIE。rt_hw_interrupt_enable 本来就是 4 字节对齐，留在 SDK。
 */
    .section .text.hmi_irq_guard, "ax", %progbits
    .balign 64
    .globl  __wrap_rt_hw_interrupt_disable
    .type   __wrap_rt_hw_interrupt_disable, %function
__wrap_rt_hw_interrupt_disable:
    csrrci  a0, mstatus, 8
    ret
    .size   __wrap_rt_hw_interrupt_disable, . - __wrap_rt_hw_interrupt_disable
