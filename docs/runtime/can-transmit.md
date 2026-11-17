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

The full write result is the only available completion success indication. Zero plus a non-BUS_OFF status does not prove successful transmission. Nor does failure prove that the frame was never put on the wire, so Framework must not replay it. Changing baud enters/exits hardware reset; only the stuck watchdog uses it once after 2 s of continuous failure, never as a per-frame retry.

The native mechanism does exactly this: on an error interrupt it updates state from `CAN_STAT`, and on BUS_OFF it reports TX_FAIL and clears `MODE.RST`. It does not rewrite mode bits, refresh software state or implement any timeout. With a persistent bus fault, or after a warm reset that leaves the peripheral domain running with `SLEEP`/`LISTEN` still set, the controller stays unwritable and stops raising interrupts, so Framework has to recover on its own.

**SDK patch**: `phandle->status.current_state` was only written from the error interrupt, so a controller reset left the software state untouched, and a recovered bus raises no further error interrupt — the state stayed BUS_OFF permanently. This SDK calls `hal_can_sync_status()` at the end of `hal_can_release_reset_mode()` to re-sample `CAN_STAT`. The code is marked `AIC-SDK-PATCH(can-status-resync)`; keep or reapply it when porting a newer SDK, otherwise `RT_CAN_CMD_GET_STATUS` returns a stale `errcode` after a reset. The patch alone still does not guarantee recovery: `CAN_STAT` is sampled the instant `RST` clears, and the controller may not have rejoined the bus yet, so the probe write below remains necessary.

## Framework policy

One existing TX worker per declared bus serializes writes, configuration and permanent stop. There are no extra threads or retry queues.

- Opening a bus issues `SET_MODE(NORMAL)` before setting the rate: a warm reset does not reset the peripheral domain, so `LISTEN`/`SLEEP` from a previous session survive it, and `hal_can_init()` only touches `MODE.RST`.
- A status query before submission rejects BUS_OFF or unavailable status without another hardware attempt; it only accumulates stuck time. After 2 s of continuous failure the watchdog resets the controller once (`SET_MODE(NORMAL)` + `SET_BAUD`, which rewrites the bit timing and leaves reset), with a 2 s minimum interval; any successful send clears it. The reset runs inside that bus's TX worker, serialized with writes and rate changes. It is not a per-error-count retry and never replays a failed frame.
- After a reset one **probe write** is allowed: it skips the status gate, writes the hardware mailbox once, and is withdrawn afterwards regardless of outcome. This is required because the status query reads a driver-side cache that may still say BUS_OFF after the reset; returning early on it produces no interrupt and no state refresh, so the controller never leaves bus-off. A successful probe clears the counters; a failed one restarts the timing.
- Full writes increment success. Zero/short writes and unavailable/BUS_OFF status increment failure. Failure advances to the next queue entry immediately, with no 100–1000 ms sleep or automatic retransmission. After an ordinary-frame failure the worker drops ordinary frames for a per-bus backoff window (50 ms, doubling per consecutive failure, capped at 200 ms) without calling the driver; explicit priority frames still attempt immediately. The window clears on the next success. This keeps a stuck hardware buffer from making the high-priority TX worker repeat the SDK HAL rejection log and starve the lower-priority UI thread. Per-bus Framework error summaries emit on the first error and at most once per second thereafter; counters retain every failure. HAL/driver diagnostics retain their SDK logging policy.
- Explicit protocol priority precedes ordinary backlog. Runtime priority 17 precedes TX priority 18, so backlog cannot prevent protocol scheduling. Update traffic retains its four-frame burst quota.
- Expiry, session and optional protocol transaction validation run before driver submission; expiry/session/stop are rechecked after validation. Expired or revoked frames are dropped without calling the driver. The protocol owns timeout, cancellation and Abort; the worker never advances an SDO state machine.
- Stop rejects new submissions and drops queued frames. After the current driver call returns, native LISTEN mode ends bus participation until restart.

## Timing and verification

The worker adds no failure sleep before a newly queued priority frame; the ordinary-frame backoff only drops frames, it does not block the worker or delay priority submissions. A periodic schedule can keep its phase and skip missed periods. Already submitted hardware writes cannot be preempted or recalled. A driver call can wait up to 1000 ticks (1000 ms at 1 kHz), so a strict wire-period guarantee during missing completion or bus faults is outside this Framework change. When the controller is completely idle (stale SLEEP/LISTEN, or the bus held dominant), the first frame is accepted by the hardware but completion waits the full 1000 ticks and the transmit buffer then stays busy until the 2 s watchdog resets it — that difference also distinguishes "no ACK" (fails immediately) from "never started transmitting". Prove healthy-bus cadence and recovery on the board with timestamped CAN capture; host cadence tests establish scheduling and queue policy only.

`can-execution` executes the real worker with controlled queues and hardware: ordinary and checked failures, priority under backlog, ordinary-frame backoff and clearing, repeated-failure log aggregation, expiry during validation, cancellation, session changes, configuration and stop. `can-port` executes the real port: complete/zero/short result, status failure, BUS_OFF suppression, forced NORMAL mode on open, one reset after 2 s of continuous failure with its minimum interval, the post-reset probe write (admitted on success, withdrawn on failure, exactly once), clearing on success, warning/passive operation, baud changes and LISTEN. These tests do not emulate completion IRQ timing or certify electrical recovery.
