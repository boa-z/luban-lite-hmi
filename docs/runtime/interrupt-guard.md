# Interrupt-disable guard

> [简体中文](interrupt-guard.zh-CN.md)

## Why it exists

On a D21x board with the same firmware image, two crashes happened on the idle main screen. Both were in
the LVGL thread, with plenty of stack left, and both trace to the first instruction of the SDK's
`rt_hw_interrupt_disable`: `csrrci a0, mstatus, 8` at `0x4000033e`.

- `mcause=2` (illegal instruction) with `mepc=0x4000033e`. The instruction is legal in M-mode.
- `mcause=5` (load access fault) on a valid `.bss` address. The same `csrrci` had not written `a0`, so
  `rt_sem_take` kept the semaphore pointer as `level` and `rt_hw_interrupt_enable` wrote it into
  `mstatus`. The saved `mstatus` equals that pointer after WARL legalisation, and the next data load
  faulted. That the fault comes from `MPRV` with an illegal `MPP` is an inference.

The source is two instructions and correct. What the crashes share is the position: the 4-byte
instruction follows `.pendsv_exit` on a 2-byte boundary and straddles the 64-byte cache line at
`0x40000340`. It is the only CSR instruction in the image that does so. Whether the straddle is the
cause is not proven; a supply or timing margin problem fits the facts as well.

## What it does

With `AIC_HMI_IRQ_GUARD` (default `y`) the application links `-Wl,--wrap=rt_hw_interrupt_disable`
and `platform/rtthread/hmi_irq_guard.S`. Calls to the SDK function resolve to
`__wrap_rt_hw_interrupt_disable`, a copy of the same two instructions that starts on a 64-byte boundary.
The SDK source and `rt_hw_interrupt_enable` are unchanged.

## Comparing the two layouts on a board

`AIC_HMI_IRQ_STRESS` (default `y`, about 1 KB) adds one shell command:

```
hmi_irq_stress <real|aligned> [seconds]
```

`real` runs the SDK copy as linked, `aligned` runs the guard copy. Every other call site in the image
uses the guard copy, so `real` exposes only the stress loop to the SDK layout. Each call presets `a0` with
a marker and checks what comes back. Calls run back to back for one tick, then the command sleeps 1 ms.
The command lowers its own priority below the watchdog supervisor while it runs, so a runaway loop cannot
starve the feed and be mistaken for a CPU lock-up. Run it with the UI up so display interrupts are active.
A result line, printed every 2 seconds, reports iterations and these counters:

| Counter | Meaning |
|---|---|
| `unwritten` | `a0` still held the marker: `csrrci` did not write back |
| `no_mie` | the returned level had `MIE` clear |
| `lost_mie` | `MIE` was still clear after `rt_hw_interrupt_enable` |
| `tick_stalls` | the tick did not advance for about 0.8 s; `mstatus`, `mie` and `mip` are printed |
| `mhz` | core clock from `mcycle` against the 4 MHz timer, measured only during the bursts |
| `per_ms` | iterations per millisecond of burst time |
| `irq` | the two interrupt sources whose count grew most in the period, as `#number+increase` |

A drop in `per_ms` is explained by the other two numbers. If `mhz` halves, the core clock changed. If `mhz`
holds and `per_ms` halves, interrupts or other threads took the CPU, and `irq` names the busiest sources.
`list_irq` maps the numbers to handlers; GPIO pins are numbered after the last peripheral interrupt.

An illegal-instruction trap cannot be counted. The shell prints the last counters from the trap handler
instead, so note `iterations` and `elapsed_ms` of that line.

A reset with no trap dump is read from the boot log: `Reset flag: 0x400` with `Watchdog-Reset` means the
hardware watchdog expired, bit 0 is power-on and bit 8 is the external pin. With the stress below the
supervisor, a watchdog reset means the CPU or the interrupt path stopped, not that the supervisor was
starved.

| Observation | Reading |
|---|---|
| `real` fails and `aligned` does not, over comparable time | the layout is the trigger; keep the guard |
| both fail | not the layout; keep the counters as evidence for a hardware margin check |
| neither fails | inconclusive; the original failures appeared after long idle runs |

Turn `AIC_HMI_IRQ_STRESS` off for release builds. Keep `AIC_HMI_IRQ_GUARD` on while the cause
is open.

## Lockup sentinel

A watchdog reset leaves only `Reset flag`, which does not say who held the CPU. With
`AIC_HMI_LOCKUP_SENTINEL` (default `y`) a heartbeat thread `hmi_hb` runs at the supervisor's
priority, and a hook in the tick interrupt watches it. Twenty seconds after boot, when the heartbeat has
stopped for 3 s, which is before the 8 s hardware timeout, the hook prints up to six reports, one per second:

```
lockup sentinel: heartbeat starved 3000 ms, running=hmi_nvm prio=21 mepc=40012345 mstatus=a00007880
  irq since snapshot (850 ms ago): #7 +850, #41 +12 (list_irq shows the numbers)
```

`running` is the thread holding the CPU and `mepc` is the interrupted instruction. Resolve it with
`riscv64-unknown-elf-addr2line -f -e d21x.elf <mepc>`. A `heartbeat resumed` line means the stall was
transient. No output at all before a watchdog reset means the interrupts stopped too: a hard hang, or code
that spins with `MIE` clear.
