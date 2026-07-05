#!/usr/bin/env python3
"""Parse an ESP32 partition-table binary and compare to esp32-flasher.json."""

from __future__ import annotations

import argparse
import json
import struct
import sys
from pathlib import Path

PART_MAGIC = 0x50AA
END_MARKER = b"\xff" * 32

APP_SUBTYPES = {
    0x00: "factory",
    0x10: "ota_0",
    0x11: "ota_1",
    0x12: "ota_2",
}
DATA_SUBTYPES = {
    0x00: "ota",
    0x01: "phy",
    0x02: "nvs",
    0x03: "coredump",
    0x82: "spiffs",
}


def subtype_name(ptype: int, subtype: int) -> str:
    if ptype == 0:
        return APP_SUBTYPES.get(subtype, f"0x{subtype:02x}")
    if ptype == 1:
        return DATA_SUBTYPES.get(subtype, f"0x{subtype:02x}")
    return f"0x{subtype:02x}"


def parse_partition_bin(data: bytes) -> list[dict]:
    entries: list[dict] = []
    for offset in range(0, len(data), 32):
        chunk = data[offset : offset + 32]
        if len(chunk) < 32:
            break
        if chunk == END_MARKER:
            break
        magic, ptype, subtype, poff, psize = struct.unpack_from("<HBBII", chunk, 0)
        if magic != PART_MAGIC:
            break
        label = chunk[12:28].split(b"\x00", 1)[0].decode("ascii", errors="replace")
        entries.append(
            {
                "name": label,
                "type": "app" if ptype == 0 else "data",
                "subtype": subtype_name(ptype, subtype),
                "offset": f"0x{poff:x}",
                "size": f"0x{psize:x}",
            }
        )
    return entries


def load_json_bootstrap(spec_path: Path) -> list[dict]:
    spec = json.loads(spec_path.read_text(encoding="utf-8"))
    return spec["boards"][0]["partitions_bootstrap"]["entries"]


def entries_match(actual: list[dict], expected: list[dict]) -> list[str]:
    errors: list[str] = []
    if len(actual) != len(expected):
        errors.append(f"entry count {len(actual)} != {len(expected)}")
    for i, exp in enumerate(expected):
        if i >= len(actual):
            errors.append(f"missing entry {exp['name']}")
            continue
        act = actual[i]
        for key in ("name", "type", "subtype", "offset", "size"):
            if act.get(key) != exp.get(key):
                errors.append(
                    f"{exp['name']}.{key}: compiled {act.get(key)!r} != json {exp.get(key)!r}"
                )
    return errors


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("partition_bin", type=Path, help="*.ino.partitions.bin path")
    parser.add_argument(
        "--check-bootstrap",
        action="store_true",
        help="Compare against esp32-flasher.json partitions_bootstrap",
    )
    parser.add_argument("--json", type=Path, help="esp32-flasher.json path")
    args = parser.parse_args()

    data = args.partition_bin.read_bytes()
    entries = parse_partition_bin(data)

    if args.check_bootstrap:
        root = Path(__file__).resolve().parents[1]
        json_path = args.json or (root / "esp32-flasher.json")
        expected = load_json_bootstrap(json_path)
        errors = entries_match(entries, expected)
        if errors:
            for err in errors:
                print(f"error: {err}", file=sys.stderr)
            return 1
        print(f"partitions_bootstrap OK ({len(entries)} entries)")
        return 0

    print(json.dumps(entries, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
