# Production runtime

> [中文版](runtime-production.zh-CN.md)

## Threads

One runtime thread owns the Domain (Core), the protocol adapters and every Product callback. Separate threads exist only where a call can block on hardware.

| Thread | Owns / does | Wakes on | Stack |
|---|---|---|---|
| `hmi_app`, priority 17 | Core, protocol routing and adapters, Product `app_run`/`app_tick`/`local_action`, periodic scheduling, snapshot publication | CAN RX, UI action, or 5 ms | 12288 bytes |
| `hmi_tx0` / `hmi_tx1`, priority 18 | One per opened bus; the only caller of the CAN driver write | Its queue | 2048 bytes each |
| `hmi_ui`, LVGL priority + 2 | LVGL, Product UI | 16 ms | 32768 bytes by default |
| `hmi_wdog`, priority 26 | Hardware watchdog feed | 200 ms | 2048 bytes |

All Product callbacks except the UI factory and pre-send valid check run serially on the runtime thread. Products must protect state shared with UI submissions or TX queries. The host simulator runs the same sequence in one loop through `hmi_runtime_step()`.

## One runtime iteration

2. Read up to 64 frames from the opened buses. Each frame is routed and decoded at once; decoders write straight into Core.
3. Apply queued UI/MSH actions (`HMI_ACTION_PRODUCT` goes to `local_action`, the rest to Core).
4. `hmi_runtime_step()`: adapter `process`, Product `app_run` with the command port, signal timeouts, Product `app_tick`.
5. Encode due periodic frames from the current snapshot and queue them.
6. Apply the brightness setting to the backlight.
7. Publish: copy the snapshot for the UI when its revision changed, copy diagnostics for MSH.

After each iteration, the runtime sleeps for 1 ms if it drained 64 frames or if its active window reached 5 ms. The window restarts after this sleep or a timed-out semaphore wait. Repeated receive wakeups can keep the semaphore ready even on batches smaller than 64 frames, so the time limit gives the UI and watchdog a scheduling opportunity under that load.

## Interfaces between threads

| From → to | Mechanism | Depth |
|---|---|---|
| UI / MSH → runtime | `hmi_execution_action()`; returns false only when the queue is full or settings are not accepted | 8 actions |
| runtime → UI | `hmi_execution_present()`; the UI copies under the publication lock only when the revision changed | one published copy |
| runtime → TX | Ordinary and priority queues | 16 + 16 per bus |

The publication lock (`hmi_debug_lock`) also guards the public diagnostics, so `hmi domain` and `hmi signal` read the same copy as the UI.

## Checked transmission and device configuration

send_checked copies optional priority, expiry, transaction tag and a read-only valid callback into the existing queues. The TX worker calls valid just before the driver; it may inspect state but must not perform I/O, mutate the protocol stack or submit another send. context must outlive queued references. Session changes, expiry, transaction cancellation and permanent shutdown can invalidate frames. All sends continue to the next queue entry after failure without a worker-wide sleep; after an ordinary-frame failure the worker drops ordinary frames for a 50–200 ms per-bus backoff window without driver calls, while priority frames still attempt immediately. Runtime priority 17 precedes TX priority 18 so an ordinary backlog cannot delay protocol enqueueing. An in-progress hardware write cannot be preempted.

Queue acceptance does not establish hardware success. Only a full RT-Thread write counts as TX success; zero/short writes and status-query failures count as errors. BUS_OFF writes are skipped and only accumulate stuck time; after 2 s of continuous failure a watchdog resets the controller once (2 s minimum interval, cleared by any success), and opening a bus forces NORMAL mode first. Framework adds no per-frame retry and no 100–1000 ms sleep; the ordinary-frame backoff drops frames instead of sleeping or replaying them. Per-bus error summaries are limited to one per second. Expiry is checked again after transaction validation. See [CAN driver behavior](can-transmit.md) for completion, recovery and timing limits.

The runtime repeatedly polls configure for the same request. The sole TX worker serializes bitrate changes and publishes success/failure without blocking runtime on a device lock. A new different bitrate may replace an unclaimed completed result, but cannot replace an executing request. The Product owns storage and subsequent device notification. stop rejects new queue entries and drops queued frames, then takes the native controller offline after the current write returns, until reboot.

## Commands

`hmi_command_port_t.send()` routes a command through the Product's `command_route` to the owning adapter's `command()` and returns whether the adapter accepted it, in the same call. Transactions that wait for an answer (SDO, parameter read/write, version query) live in the adapter: it sends frames in `process`, matches answers in `on_frame`, applies its own timeouts and publishes results as signals.

## Periodic frames

A `hmi_periodic_frame_t` names the bus, identifier, length and period; `encode(snapshot, now, frame)` fills the data from the live snapshot when the deadline arrives. Freshness is the encoder's decision, based on each signal's state. One frame per entry may wait in the TX queue; a frame that has not reached the driver within one period is dropped, so a bus without acknowledgement never builds a backlog. Late deadlines skip missed periods instead of bursting.

## Sessions

Runtime startup begins a session, advances the generation, marks old signals stale, resets adapters and calls the Product’s `app_reset`. The TX queues continue to check the generation.

## Startup

The RT-Thread main thread composes the selected Product, calls its preparation and startup hooks, and starts the runtime. The runtime applies boot settings, opens the buses used by the Product, and publishes its initial snapshot. An SDK LVGL timer attaches the Product UI after that snapshot is available. A UI initialization failure is logged and the runtime continues. The hardware watchdog starts supervising the app after successful runtime startup. Products can opt in to UI supervision after the first successful presentation.

Products own persistence. The optional `runtime_init` hook restores boot state before opening CAN buses. The optional `action_applied` hook lets a Product schedule a save after an accepted local action.

## Diagnostics

`hmi_exec` prints the generation, rejected actions, TX queue-full count, per-bus TX ok/error/dropped counters and per-entry periodic misses and lateness. `hmi can`, `hmi runtime`, `hmi domain` and `hmi trace` are described in [diagnostics](diagnostics.md).

## Build-time Product selection

Each firmware image contains exactly one Product, selected in SCons by `CONFIG_AIC_HMI_PRODUCT_ROOT` in the defconfig; the `HMI_PRODUCT_ROOT` environment variable overrides it for one build. An empty selection stops the firmware build. Host CMake builds the Framework alone unless `-DHMI_PRODUCT_ROOT` names a Product. The selected `product/sources.json` supplies the firmware composition (`contracts/hmi_firmware.h`): three non-overlapping storages (runtime Domain, published copy, UI copy) and the locale setup.
