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

The full write result is the only available completion success indication. A failed write leaves its wire outcome uncertain. Framework advances to the next queued frame without replaying it.

On BUS_OFF the native mechanism reports TX_FAIL and clears `MODE.RST`. If mode bits remain set when the bus should be in NORMAL, the Framework can restore mode and bit timing.

`RT_CAN_CMD_GET_STATUS` returns a software cache updated by error interrupts. Its BUS_OFF value can outlast hardware recovery. The ArtInChip CAN v1 port reads `CAN_STAT.BUS` before each submission, and the HAL checks the same hardware bit before issuing a BUS_OFF `TX_FAIL` callback or starting automatic recovery.

## Framework policy

One existing TX worker per declared bus serializes writes, configuration and permanent stop. There are no extra threads or retry queues.

- The ArtInChip CAN0 HAL holds the controller in reset until its first rate is set. On `can0`, the port programs the rate before setting NORMAL and enabling interrupts. Other devices retain the NORMAL-before-rate sequence.
- ArtInChip CAN v1 submits only when hardware is out of BUS_OFF and its TX buffer is writable. The hardware handles automatic BUS_OFF recovery. The Framework rewrites mode and timing only when failed sends continue to observe abnormal mode bits for at least 2 s. Ports without a hardware status mapping use reported BUS_OFF as the recovery evidence and allow one probe write after a reset or rate change. Any success or loss of the fault indication clears the timer. The TX worker serializes recovery with writes and rate changes.
- Full writes increment success. Zero/short writes and unavailable/BUS_OFF status increment failure. Failure advances to the next queue entry immediately, with no 100–1000 ms sleep or automatic retransmission. After an ordinary-frame failure the worker drops ordinary frames for a per-bus backoff window (50 ms, doubling per consecutive failure, capped at 200 ms) without calling the driver; explicit priority frames still attempt immediately. The window clears on the next success. This keeps a stuck hardware buffer from making the high-priority TX worker repeat the SDK HAL rejection log and starve the lower-priority UI thread. Per-bus Framework error summaries emit on the first error and at most once per second thereafter; counters retain every failure. HAL/driver diagnostics retain their SDK logging policy.
- Explicit protocol priority precedes ordinary backlog. Runtime priority 17 precedes TX priority 18, so backlog cannot prevent protocol scheduling.
- Expiry, session and optional protocol transaction validation run before driver submission; expiry/session/stop are rechecked after validation. Expired or revoked frames are dropped without calling the driver. The protocol owns timeout, cancellation and Abort; the worker never advances an SDO state machine.
- Stop rejects new submissions and drops queued frames. After the current driver call returns, native LISTEN mode ends bus participation until restart.

## Timing and verification

The worker yields for 1 ms after 5 ms of continuous queue service and waits for new work when idle. Already submitted hardware writes cannot be preempted or recalled. A missing completion can still wait up to 1000 ticks (1000 ms at 1 kHz). Use `hmi_exec` to compare board send attempts, returns, and maximum duration per bus. Prove healthy-bus cadence and recovery on the board with timestamped CAN capture. Host tests establish scheduling and queue policy.

`can-execution` covers queue policy, the RX time and frame budgets, TX attempt/return counts, transactions, configuration and stop. `can-port` covers driver results, BUS_OFF fallback recovery and its minimum interval, recovery suppression without fault evidence, the single probe write, baud changes and LISTEN. Completion IRQ timing and electrical recovery require board tests.
