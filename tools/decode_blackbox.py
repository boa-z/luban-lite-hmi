import argparse
import json
import struct
import sys
import zlib
from pathlib import Path


SECTOR = 512
SLOTS = 4096
HEADER = struct.Struct("<7I32s")
OWNERS = ("app", "ui", "tx0", "tx1", "sdo", "nvm")
STAGES = ("unseen", "idle", "startup", "can_read", "can_dispatch", "actions", "protocol",
          "periodic", "backlight", "publish", "ui_snapshot", "ui_render", "ui_diag", "ui_log",
          "can_write", "can_configure", "sdo_upload", "sdo_download", "nvm_write")


def decode_sector(sector, slot):
    magic, schema, sequence, boot, uptime, reset_hw, reset_sdk, firmware = HEADER.unpack_from(sector)
    if (magic != 0x31424248 or schema != 1 or not 0 < boot <= sequence
            or (sequence - 1) % SLOTS != slot or zlib.crc32(sector[:-4]) != int.from_bytes(sector[-4:], "little")):
        return None
    offset = HEADER.size

    def fields(names):
        nonlocal offset
        keys = names.split()
        values = struct.unpack_from(f"<{len(keys)}I", sector, offset)
        offset += 4 * len(keys)
        return dict(zip(keys, values))

    owners = {}
    for name in OWNERS:
        owner = fields("stage entered_ms detail longest_ms longest_stage heartbeat heartbeat_ms "
                       "completed last_detail result code")
        for key in ("stage", "longest_stage"):
            value = owner[key]
            owner[key] = STAGES[value] if value < len(STAGES) else value
        owners[name] = owner
    watchdog = fields("feed_ms state state_ms channel feed_errors")
    buses = [fields("available sampled_ms state tx_errors ack_errors recovery_ok recovery_failed "
                    "rx tx rx_drop tx_drop rx_error tx_error tx_queue_full rx_budget_hits last_rx_ms last_tx_ms")
             for bus in range(2)]
    sdo_failure = fields("at_ms detail code")
    nvm_failure = fields("at_ms detail code")
    return {"sequence": sequence, "boot": boot, "uptime_ms": uptime, "reset_hw": reset_hw,
            "reset_sdk": reset_sdk, "firmware": firmware.split(b"\0", 1)[0].decode(errors="replace"),
            "owners": owners, "watchdog": watchdog, "can": buses, "sdo_failure": sdo_failure,
            "nvm_failure": nvm_failure}


def main():
    parser = argparse.ArgumentParser(description="Decode CRC-valid HMI black box sectors as sorted JSON Lines")
    parser.add_argument("ring", type=Path)
    args = parser.parse_args()
    records = []
    damaged = 0
    with args.ring.open("rb") as stream:
        for slot in range(SLOTS):
            sector = stream.read(SECTOR)
            if not sector:
                break
            if len(sector) != SECTOR:
                damaged += 1
                break
            record = decode_sector(sector, slot)
            if record is not None:
                records.append(record)
            elif any(sector):
                damaged += 1
    print(f"{len(records)} valid sectors, {damaged} invalid or partial sectors", file=sys.stderr)
    for record in sorted(records, key=lambda item: item["sequence"]):
        print(json.dumps(record))
    return 0 if records else 1


if __name__ == "__main__":
    raise SystemExit(main())
