# Watchdog supervision

> [简体中文](watchdog.zh-CN.md)

## Ownership

`runtime/hmi_watchdog.c` owns the judging rule and nothing else: it takes a heartbeat counter per
owner plus the set of owners that should be advancing right now, and answers whether the hardware
watchdog may be fed. It opens no device, creates no thread and holds no lock, so a host test can
drive every branch. `platform/rtthread/hmi_watchdog_port.c` owns the board half: the `"wdt"`
device, one lowest-priority thread, the keepalive call and the `hmi_watchdog` shell command.

The watch list is the two resident threads that must keep running for the application to stay
truthful: the runtime thread (`app`) and the UI. Counters come from `hmi_execution_liveness()`,
which reads their loop counters. The watchdog never inspects another owner's internals.

## When the watchdog is armed

| Execution state | Supervised | Hardware watchdog |
|---|---|---|
| Runtime not started (bring-up) | no | not armed |
| Runtime started | app | armed |
| First successful presentation, with Product UI supervision enabled | app and UI | armed |
| Startup failed | no | not armed |

The hardware watchdog is armed the first time the supervisor observes a started runtime. Before that
the port only polls, so a slow boot (for example a long EEPROM initialization) cannot be reset. There
is no runtime stop, so once armed the watchdog stays armed until reset.

Bring-up itself is therefore outside the supervised window. A hidden fatal condition before the
runtime starts is not converted into a reset by this mechanism; it is reported by the start-up path
and by `hmi` diagnostics. Covering bring-up needs a boot-progress channel fed by the entry point,
which is deliberately not part of this contract.

## Thresholds

Each channel has its own stall limit, so a slow owner can be tolerated without weakening the others.
A channel is judged only while it is expected; when it leaves the expected set its baseline is
dropped, and re-entering always restarts the window, so a mode change cannot create an instant false
stall.

| Budget | Default | Meaning |
|---|---|---|
| `HMI_WATCHDOG_TIMEOUT_S` | 8 | Hardware timeout in seconds |
| `HMI_WATCHDOG_POLL_MS` | 200 | Supervisor sampling period |
| `HMI_WATCHDOG_APP_STALL_MS` | 4000 | Runtime-thread heartbeat limit |
| `HMI_WATCHDOG_UI_STALL_MS` | 3000 | UI heartbeat limit |
| `HMI_WATCHDOG_THREAD_PRIO` | 26 | Supervisor priority, below every supervised owner |

All of them live in `platform/rtthread/hmi_watchdog_budget.h` and are `#ifndef`-overridable, so a
board retunes them without touching the judging rule. The enable switch is the Kconfig option
`AIC_HMI_WATCHDOG`, default on; turning it off compiles the port down to a logging stub. A board
without a `"wdt"` device logs once and stays unsupervised instead of failing to boot.

The supervisor thread runs at a lower priority than every supervised owner, so it is only scheduled
when the others have yielded. A higher-priority busy loop starves the supervisor and the hardware
reset follows, which is the intended behaviour rather than a side effect.

## Diagnostics

On the current single-core targets, the RT-Thread scheduler critical section protects state transitions
and report copying. Diagnostics read the authoritative state directly; no duplicate counters are maintained.
Device I/O and logging stay outside the critical section. The report is for thread callers, including MSH.

```
hmi_watchdog
watchdog armed=1 supervising=1 timeout_s=8
watchdog feeds=812 withheld=0 stalls=0 transitions=1 stalled=2
watchdog channel=app stall_ms=4000
watchdog channel=ui stall_ms=3000
```

`withheld` counts samples where the feed was refused and `stalls` counts the transitions into a
stalled state; `stalled` is the first stalled channel index, or the channel count when healthy. A
non-zero `withheld` without a reset means the watchdog recovered before the hardware timeout, which
is the intended margin between the 3-4 s stall limits and the 8 s hardware timeout.

## Limits

The stall limits are engineering budgets, not measured worst cases. Target evidence for the slowest
observed owner period, for the longest legitimate blocking section and for a deliberate fault
injection is still required before a safety claim. Register write protection
(`RT_DEVICE_CTRL_WDT_EN_REG`) is deliberately left off, because this framework has no hardware
evidence yet that keepalive still works with it enabled.

See [runtime production](runtime-production.md) for the owner list and their priorities.
