# Protocol and Domain contracts

## Domain revision

Core revision compares complete `hmi_value_t` values (timestamp, state, source and value); parameter, fault, connection, settings and profile changes also advance it. The UI copies the published snapshot only when the revision changed. A signal `stale_ms` of zero disables the automatic timeout. Source arbitration receives the complete current and incoming values.

## Protocol service boundary

Receive routing maps (bus, identifier, format) to one owner. An owner is either a stateless `hmi_decode_fn_t` (generated from a DBC) or a stateful `hmi_protocol_adapter_t` with `on_frame`, `process`, `command` and `reset`. `hmi_protocol_services_t` carries the Domain update sink, the CAN TX port and optional diagnostics; it is valid only during the callback. Decoders write straight into Core on the runtime thread.

Signals are continuous state; commands are business requests from the App. `hmi_command_port_t.send()` hands a command to the adapter chosen by the Product's `command_route` and returns whether the adapter accepted it. Request/response transactions, their timeouts and retries are adapter state; results reach the UI as signals. Runtime has no singleton transaction, no request ids and no transport timeout of its own.

Core knows no CAN; UI knows no CAN; Runtime has no Product branches.

## Demo instrument transmit PDOs

The Demo Product publishes two public synthetic standard CAN0 data frames at 100 ms, DLC 8, little endian. The transmit-only definition is `products/demo/protocol/can/demo_tx.dbc`. These examples do not implement CANopen NMT, SYNC, heartbeat, configurable PDO mapping or an object dictionary, and are not vehicle control commands.

| Frame | Bytes / bits | Meaning |
|---|---|---|
| 0x381 demo_tpdo_motion | 0–1 / 2–3 / 4–5 / 6 | Speed 0.01 km/h / lift height 0.001 m / load kg / SOC percent |
| 0x381 demo_tpdo_motion | byte 7 bit 0 / bits 1–7 | Whole-group fresh / 7-bit sequence |
| 0x481 demo_tpdo_status | byte 0 bits 0–4 | Seat occupied / brake active / neutral / charging / warning |
| 0x481 demo_tpdo_status | byte 1 bit 0 / byte 2 / byte 3 | Whole-group fresh / 8-bit sequence / layout version 1 |
| 0x481 demo_tpdo_status | bytes 4–7 | Runtime session generation, not a firmware version or persistent counter |

A group is fresh only when every source signal is VALID and in range. Otherwise it sends zero values and fresh=0, while valid zero stays fresh=1. Sequences advance on every encoding. Both frames use the ordinary TX queue. `demo-pdo` checks the golden vectors and freshness rules. `demo-pdo-dbc` decodes the actual C output with cantools.
