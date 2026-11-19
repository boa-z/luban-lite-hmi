# Diagnostics and serial debugging

## Responsibility and ownership

Log is for important human-readable events, Diagnostics for current state/counters, and Trace for recent structured history. UART remains RT-Thread Console; FinSH/MSH owns command tokenization; ULog owns filtering, formatting and asynchronous output. No new logger, shell, UART protocol or print queue is introduced.

The portable diagnostics library depends only on public contracts. The application statically owns its diagnostics object and binds Core and Runtime; protocol services pass the optional pointer to adapters. All mutations and snapshot copies must be serialized by the caller. Unbound instrumentation is a no-op. Host programs can bind and query the same API without RT-Thread, LVGL or the SDK.

The production port publishes owner snapshots under short RT-Thread priority-mutex locks. MSH formats and writes UART after copying, outside publication locks; a separate command mutex protects query buffers. Queries do not sample devices or transmit CAN. Touch counters are copied by the LVGL owner after its timer handler.

The runtime thread owns Core and the protocol adapters; LVGL UI and CAN TX run on their own threads. The portable diagnostics object still requires caller serialization; board queries use published copies rather than concurrent access to live Core state. See runtime-production.md for thread details.

## Counters and snapshot semantics

Runtime and board-driver counters describe different boundaries. Runtime tracks accepted/dispatched frames and resets; board CAN diagnostics report physical-bus RX, driver TX success/failure and native drops. Queue admission is separate from driver completion and does not prove remote acceptance. TX busy is admission refusal, not itself a controller fault. Compare deltas over explicitly bounded observation windows.

Domain counts accepted updates, including unchanged values; revision changes only for actual Domain changes. Source and validity transitions are counted separately. Reading diagnostics or counters never increments revision. Parameters/settings/fault metadata is read from the public Domain snapshot, not private core fields. Signal lookup uses canonical keys and copies the value; key/unit strings borrow the immutable Product catalog lifetime. Age is unsigned millisecond subtraction, valid within one uint32 clock cycle. UNKNOWN is reported explicitly; its numeric age is not evidence of received data.


UI present_count is observed at presentation; flush_count counts LVGL flush-finish events of the default display. Touch exposes the pointer position, state and a press count that LVGL can see; the SDK input driver has no interrupt or read counters, so those fields stay 0. Storage reports the selected backend and RAM/in-flight/durable revisions; only absent backends report unavailable. slow_frame is not implemented. New counters saturate at UINT32_MAX; legacy Runtime counters retain their existing arithmetic.

## Trace and logging policy

The static ring holds 256 entries of 16 bytes (4096 bytes plus 16-byte metadata). Append is O(1), uses no heap and formats no strings. Snapshot returns insertion order, including uint32 timestamp wrap; short buffers select the newest entries. Overwrite count saturates. Clear removes visible history and overwrite count, retaining the consumer sequence. Queries never transmit CAN or change vehicle settings; trace clear is the only mutating command and prints the actual target/count/result.

Runtime/CAN/Domain/Product event identifiers are defined in the portable header; module numbers 3 and 4 (former PDO/SDO) stay reserved. Product READY/FAILED marks generic service outcome; it contains no customer workflow. Domain arg0 is signal identity. CAN arg0 is bus; other details remain numeric and inspectable in dump output.

Normal CAN RX/TX, Touch samples and UI frames increment counters only. State transitions/errors enter Trace. The RT-Thread backend maps selected records to native ULog tags: hmi.runtime, hmi.can, hmi.domain, hmi.product; initialization uses hmi.boot/hmi.board, and LVGL uses hmi.ui. Invalid frames and CAN errors are WARN; connection changes and product ready are INFO; product service failure is ERROR. Repeated warnings are limited to one per module per second; counters and trace still record them. Debug messages obey native ULog settings. The existing SDK uses INFO and asynchronous Console output.

The backend drains trace once per owner iteration. Ring overwrite or explicit clear can discard not-yet-consumed log events; trace/log are bounded debugging evidence, not a reliable event-delivery bus. Clear before drain may suppress that historical log. UART queries may interleave with asynchronous logs; raw capture preserves what was actually received. MSH trace copy and log-consumer copy each use another static 4096-byte buffer; total trace-related BSS is about 12 KiB, not just the history ring.

## MSH queries

```text
hmi info
hmi diag
hmi runtime
hmi_exec
hmi can
hmi can 1
hmi domain
hmi signal vehicle.speed
hmi touch
hmi trace
hmi trace dump
hmi trace clear
```

Invalid or extra arguments print usage without mutation. CAN live capture is reserved for a future bounded, filtered static buffer (bus/id/count); there is no unbounded dump hook and no RX callback UART output. signal not found is explicit; a known Demo key is vehicle.speed. info prints Product, platform SHA, SDK SHA, LVGL version, board, build date/time and uptime.

Build identity is generated by tools/build_identity.py through SCons. The generated header also names the selected Product through HMI_BUILD_PRODUCT (manifest identity.id, otherwise hmi_product_t.id) and its revision through HMI_BUILD_PRODUCT_REVISION, so an archived header states which Product composed the image. Clean repositories report full SHA; dirty repositories append a content/status fingerprint including untracked nonignored source. The board name is normalized from Kconfig. Source SHA plus image SHA256 binds evidence; build date alone is insufficient. An inaccessible repository is marked unavailable. The SDK may be dirty because a separate private submodule is dirty; do not clean another task's files just to remove this marker.

## Display colour

The Framework only loads the panel correction at start-up. `AIC_HMI_DISPLAY_GAMMA` (gamma percent,
100 = display engine default) and `AIC_HMI_DISPLAY_CCM` (12 CCM taps, empty = identity) are written
into the display engine once the display has started. The display engine applies them to the
blended picture, so the whole UI layer is corrected; the SDK brightness/contrast/saturation
adjustment only reaches the video layer. A CCM string that cannot be parsed is logged and not applied.

Panel bench tools (live gamma/CCM tuning `hmi_color`, the colour test card `hmi_testcard`, handing
the screen to the official AiPQ tool `hmi_display`, and the serial tuning GUI) live in the separate
repository [Luban-Lite Display Lab](https://github.com/boa-z/luban-lite-display-lab). Neither the
Framework nor a Product depends on it, and release firmware does not carry it. For a bench image,
clone it to `application/rt-thread/luban-lite-display-lab` and link it as an extra component for one build:

```sh
HMI_EXTRA_COMPONENTS=../luban-lite-display-lab scons
```

Bake the values found on the bench into `AIC_HMI_DISPLAY_GAMMA` / `AIC_HMI_DISPLAY_CCM` of the board defconfig.

## Host capture and evidence

```sh
python -m pip install -r tools/serial/requirements.txt
python tools/serial/hmi_serial.py --port /dev/ttyUSB0 capture evidence/serial.log
python tools/serial/hmi_serial.py --port /dev/ttyUSB0 command "hmi info" --output evidence/info.txt
python tools/serial/hmi_serial.py --port /dev/ttyUSB0 command "hmi diag" --output evidence/diag.txt
python tools/serial/hmi_serial.py --port /dev/ttyUSB0 command "hmi runtime" --output evidence/runtime.txt
python tools/serial/hmi_serial.py --port /dev/ttyUSB0 command "hmi can" --output evidence/can.txt
python tools/serial/hmi_serial.py --port /dev/ttyUSB0 command "hmi trace dump" --timeout 10 --idle 1 --output evidence/trace.txt
```

On Windows replace /dev/ttyUSB0 with the explicit COM port. Use the same Python interpreter for installing requirements and running the helper. Files append raw bytes, including ANSI escapes and malformed UTF-8; the helper does not interpret protocol success. capture runs until Ctrl-C or --duration seconds. command stops after --idle silence following data or the --timeout deadline; silence exits with failure. Long dumps need an adequate deadline. An echoed command alone is not proof that the board executed it. Control lines are configured inactive before opening, but adapter/OS behavior must be considered when reset is tied to DTR/RTS.

Only one process may own the board serial port: stop capture before running separate command invocations, or use a single external terminal with session logging. Do not run the listed capture and command processes concurrently. To capture boot, start capture before the operator resets the board. Save the whole boot log and each command response. The helper has no implicit flashing or reboot action.

Before flashing, reserve the shared board using SDK project-management policy and record its restore image. Copy the exact generated image to an evidence directory and save SHA256 (PowerShell Get-FileHash or Linux sha256sum). Save the generated build identity header, .config, image/map/ELF and SDK/platform revisions next to it. Then obtain boot log, info.txt, diag.txt, runtime.txt, can.txt and trace.txt from the physical board. Also query domain, a known signal and touch; exercise the CAN fixture. Never manufacture board logs from Host fixtures.

## Validation and limits

HOST_PASS requires the framework-only build and the architecture gate. Diagnostics rendering, the MSH frontend, the serial helper and build identity have no dedicated host tests; they are checked on the board. Linux is the CI default.

IMAGE_READY requires the selected SDK firmware build plus artifact hash. BOARD_PASS additionally requires physical boot/MSH/fixture evidence tied to that image. Current work reports BOARD_NOT_RUN when no board port is available. Persistent trace, coredump, network logging, custom UART parser and extra Demo pages are not provided.

See the [RT-Thread adapter](../../platform/rtthread/README.md) and [protocol contracts](protocols.md) for adjacent architecture.
