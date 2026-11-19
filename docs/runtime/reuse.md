# Reusing LVGL and SDK services

> [中文版](reuse.zh-CN.md)

This optimization uses the existing Luban-Lite SDK 1.3.2 and Framework, reusing the pinned LVGL, RT-Thread and ArtInChip services. Product functionality and UI use this combination; interfaces from another SDK are not prerequisites. Reuse is worthwhile when the upstream API preserves the existing contract. Device access belongs in the platform/BSP; customer wording, protocol and policy belong in the Product; contracts/Core stay independent of LVGL and RT-Thread.

## Cleanup applied

- The visual widgets (gauge, needle, ring/arc bar, linear meter, status indicator, value label, update page), their value formatting/mapping helpers and the widget vocabulary (`meter.*` state words) moved to the Demo, their only user. The Framework is a runtime platform for several Products, not an instrument widget library: `ui/common/` keeps only the translation service and the display test card, and the architecture gate rejects anything else there.
- Typed integer placeholders use libc `snprintf()` with fixed `PRId32`/`PRIu32` formats. The translator's template is still parsed as data; argument reordering, brace escaping and bounded output remain supported.
- Watchdog diagnostics copy the authoritative state under RT-Thread's scheduler critical section instead of maintaining five unsynchronised counter copies. The state transition uses the same protection. These single-core critical sections contain bounded computation/copying only; logging and device calls stay outside.

## Source-based retention decisions

SDK paths below are relative to the SDK root; Framework paths are relative to this repository.

| Area and source | Existing upstream interface | Why the remaining code stays |
|---|---|---|
| Translation: `ui/common/i18n/hmi_i18n_runtime.c`; `ui/common/i18n/lv_i18n` | lv_i18n language pack, current locale, plural rules | LVGL 9.1 has no `lv_translation`. lv_i18n supplies the lookup; label binding, language-change events, explicit-language lookup, Product-owned persisted language IDs and font roles stay in the thin adapter. |
| Display: `platform/rtthread/hmi_board_port.c`; SDK `bsp/artinchip/drv/display/` | `mpp_fb_ioctl()` and `lv_aic_init()` | Display ownership, extra UI rectangles, alpha and colour-key configuration are application composition. Camera Products can disable exclusive ownership. No duplicate framebuffer/rendering driver is needed. |
| Backlight: `platform/rtthread/hmi_board_settings.c`; SDK `bsp/artinchip/drv/display/panel/panel_com.c` | `rt_pwm_set()`, `rt_pwm_enable()`, `PWM_CMD_GET` | The panel helper enables a compile-time duty at a fixed period; it does not implement persisted brightness, the configured floor, readback or retry. Keep the existing native PWM adapter and board Kconfig values. |
| CAN: `platform/rtthread/hmi_board_port.c`, `hmi_execution_port.c`; SDK `kernel/rt-thread/components/drivers/can/can.c` | `rt_device_read/write/control()`, RX indication, CAN status | Driver TX waits for mailbox/completion. Keep per-bus workers so one blocked bus cannot stall Core, UI or the other bus. Session expiry are not provided by a raw driver queue. |
| IPC: `hmi_execution_port.c`, `hmi_nvm_port.c`; SDK `kernel/rt-thread/src/ipc.c` | Native message queues, semaphores, events and mutexes | Queues copy requests/results, one runtime owner mutates Core, and published snapshots are copied under a mutex. Replacing these with a new event bus or sharing mutable Core across threads would increase complexity. |
| RTC: `platform/rtthread/hmi_rtc_port.c`, `runtime/hmi_wall_clock.c`; SDK RTC driver and libc `ctime.c` | `time()`, `gmtime_r()`, `set_timestamp()` | UTC binding, invalid-clock filtering and the UI owner's read cache remain useful. Pure 64-bit civil conversion avoids timezone dependence and the platform's signed-32-bit write horizon. |
| Watchdog: `runtime/hmi_watchdog.c`, `platform/rtthread/hmi_watchdog_port.c`; SDK `bsp/artinchip/drv/wdt/drv_wdt.c` | RT-Thread timeout/start/keepalive controls | The device can reset the CPU but does not decide whether runtime/UI progress is healthy. Keep owner supervision and the startup gate; an idle-hook feed would mask a stalled UI. |

## HMI Framework review (2026-10-28)

The rename to Luban-Lite HMI Framework re-checked each mechanism against the SDK actually used (Luban-Lite 1.3.2 in this tree). Nothing here was rewritten on top of an SDK service; the decisions are:

| Mechanism | SDK candidate | Decision |
|---|---|---|
| Runtime thread, action queue, published snapshot (`hmi_execution_port.c`) | RT-Thread message queue, semaphore, mutex | Kept. It *is* built from native IPC; it adds only single ownership of Core and a copy under one mutex, which no SDK service provides. No event bus was added. |
| UI start-up and present loop | `lv_user_gui_init()` in the SDK LVGL thread, `lv_timer` | Kept as is: the SDK thread runs the UI, the Framework only links into it (`--wrap`) and adds an `lv_timer`. |
| Per-bus CAN transmit workers, periodic frames | RT-Thread CAN device | Kept for CAN Products; a Product without routes or periodic frames now starts none of them. The driver write blocks on the mailbox, so a stalled bus must not stall Core or the UI. |
| Asynchronous NVM worker, slots, record format | `packages/third-party/at24cxx`, DFS files | Kept. `at24cxx` allocates from the heap, fixes the chip type at compile time and has no read retry or "uncertain write" result; the Framework driver is heap-free, board-tuned and part of the power-loss-safe slot protocol. The file backend already uses DFS. |
| Backlight, display colour, RTC, watchdog, SHA-256 | PWM, DE ioctl, RTC device, WDT device, mbedTLS | Already thin adapters over the SDK drivers (see below). |
| Structured trace and diagnostics shell | ulog, MSH | Kept: ulog prints, the trace keeps a bounded binary history the shell can dump after the fact; commands are native MSH. |

Removed in this review: the Demo-only catalog parts (`hmi_monitor_def_t`, `HMI_SOURCE_DEMO`) and two functions without callers (`hmi_core_snapshot`, `hmi_board_update_started`). Not added: signal validity or timeout states, a generic HAL, plugins or a runtime Product registry.

## Current SDK reuse boundaries and acceptance

These decisions follow the current SDK source:

- `kernel/rt-thread/components/drivers/can/can.c` waits for TX mailboxes/completion; keep per-bus workers and measure both buses on the target.
- `bsp/artinchip/drv/rtc/drv_rtc.c` converts between its 2020-05-20 epoch and UTC seconds. Framework keeps the standard time interfaces and unset-clock filtering without duplicating the hardware epoch adapter.
- The backlight helper in `panel_com.c` only provides fixed initialization. The RT PWM port handles dynamic brightness and readback; Kconfig/BSP supplies board facts such as devices and channels.
- Validate watchdog start/feed/reset and timing budgets. Diagnostic snapshot protection assumes the current single-core, thread-only callers.
