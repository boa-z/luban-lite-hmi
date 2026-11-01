# CAN transmission on RT-Thread and ArtInChip

[中文版](can-transmit.zh-CN.md)

## Driver contract

These conclusions follow the SDK used by this integration. Recheck them when updating the SDK; this is not a generic RT-Thread guarantee.

| Source | Observed behavior |
|---|---|
| `kernel/rt-thread/components/drivers/can/can.c`, `_can_int_tx` | INT_TX takes a native mailbox semaphore, invokes `sendmsg`, waits for completion and returns bytes only for `RT_CAN_SND_RESULT_OK`. Rejection, TX_FAIL and completion timeout return zero for a single frame. |
| `rt_hw_can_isr` and `bsp/artinchip/drv/can/drv_can.c`, `aic_can_callback` | TX_DONE/TX_FAIL update the native result and signal completion. AIC uses one hardware transmit buffer and does not use the requested mailbox number. |
| `kernel/rt-thread/include/rtdef.h` | `RT_WAITING_TIMEOUT` is 1000 ticks. The CAN configuration's `ticks=50` controls its status timer, not this wait. |
| `bsp/artinchip/hal/can/aic_hal_can.c`, `hal_can_send_frame` | A busy hardware buffer rejects the write. Normal mode uses controller retransmission. Completion wait timeout does not abort an already submitted hardware frame. |
| `hal_can_error_handle`, `drv_can.c` initialization | BUS_OFF is AIC state 4; the HAL reports TX_FAIL and releases reset when native `autoBusoff` is enabled (the default). Error-warning interrupts update state on recovery. This does not guarantee recovery from a persistent physical fault. |

The full write result is the only available completion success indication. Zero plus a non-BUS_OFF status does not prove successful transmission. Nor does failure prove that the frame was never put on the wire, so Framework must not replay it. Changing baud enters/exits hardware reset; it is not an application retry mechanism.

## Framework policy

One existing TX worker per declared bus serializes writes, configuration and permanent stop. There are no extra threads or retry queues.

- A status query before submission rejects BUS_OFF or unavailable status without another hardware attempt. Native recovery remains responsible for the controller; Framework does not reset after a count of errors.
- Full writes increment success. Zero/short writes and unavailable/BUS_OFF status increment failure. Failure advances to the next queue entry immediately, with no 100–1000 ms sleep or automatic retransmission. Per-bus Framework error summaries emit on the first error and at most once per second thereafter; counters retain every failure. HAL/driver diagnostics retain their SDK logging policy.
- Explicit protocol priority precedes ordinary backlog. Runtime priority 17 precedes TX priority 18, so backlog cannot prevent protocol scheduling. Update traffic retains its four-frame burst quota.
- Expiry, session and optional protocol transaction validation run before driver submission; expiry/session/stop are rechecked after validation. Expired or revoked frames are dropped without calling the driver. The protocol owns timeout, cancellation and Abort; the worker never advances an SDO state machine.
- Stop rejects new submissions and drops queued frames. After the current driver call returns, native LISTEN mode ends bus participation until restart.

## Timing and verification

The worker adds no failure sleep before a newly queued priority frame. A periodic schedule can keep its phase and skip missed periods. Already submitted hardware writes cannot be preempted or recalled. A driver call can wait up to 1000 ticks (1000 ms at 1 kHz), so a strict wire-period guarantee during missing completion or bus faults is outside this Framework change. Prove healthy-bus cadence and recovery on the board with timestamped CAN capture; host cadence tests establish scheduling and queue policy only.

`can-execution` executes the real worker with controlled queues and hardware: ordinary and checked failures, priority under backlog, zero added failure delay, repeated-failure log aggregation, expiry during validation, cancellation, session changes, configuration and stop. `can-port` executes the real port: complete/zero/short result, status failure, BUS_OFF suppression and native recovery, warning/passive operation, baud changes and LISTEN. These tests do not emulate completion IRQ timing or certify electrical recovery.
