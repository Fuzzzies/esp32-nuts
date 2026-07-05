#!/usr/bin/env python3
"""Validate esp32-flasher.json partition/NVS consistency."""

from __future__ import annotations

import json
import re
import sys
from pathlib import Path

HEX_RE = re.compile(r"^0x[0-9a-fA-F]+$")


def parse_hex(value: str, label: str) -> int:
    if not isinstance(value, str) or not HEX_RE.match(value):
        raise ValueError(f"{label}: expected hex string like 0x9000, got {value!r}")
    return int(value, 16)


def find_nvs_entry(entries: list[dict]) -> dict:
    for entry in entries:
        if entry.get("name") == "nvs":
            return entry
    raise ValueError("partition entries missing nvs row")


def validate_nvs_block(nvs_cfg: dict, entries: list[dict], label: str) -> None:
    nvs_part = find_nvs_entry(entries)
    part_offset = parse_hex(nvs_part["offset"], f"{label} nvs.offset")
    part_size = parse_hex(nvs_part["size"], f"{label} nvs.size")
    cfg_offset = parse_hex(nvs_cfg["offset"], f"nvs.{label}.offset")
    cfg_size = parse_hex(nvs_cfg["size"], f"nvs.{label}.size")
    if cfg_offset != part_offset:
        raise ValueError(
            f"nvs.{label}.offset ({nvs_cfg['offset']}) "
            f"!= partitions_{label} nvs offset ({nvs_part['offset']})"
        )
    if cfg_size != part_size:
        raise ValueError(
            f"nvs.{label}.size ({nvs_cfg['size']}) "
            f"!= partitions_{label} nvs size ({nvs_part['size']})"
        )


def main() -> int:
    root = Path(__file__).resolve().parents[1]
    spec_path = root / "esp32-flasher.json"
    if not spec_path.is_file():
        print(f"error: missing {spec_path}", file=sys.stderr)
        return 1

    spec = json.loads(spec_path.read_text(encoding="utf-8"))
    boards = spec.get("boards")
    if not isinstance(boards, list) or not boards:
        print("error: boards must be a non-empty array", file=sys.stderr)
        return 1

    errors: list[str] = []
    for board in boards:
        board_id = board.get("id", "<unknown>")
        try:
            full_entries = board.get("partitions_full", {}).get("entries", [])
            if not full_entries:
                raise ValueError("partitions_full.entries is empty")
            validate_nvs_block(board["nvs"]["full"], full_entries, "full")

            bootstrap_entries = board.get("partitions_bootstrap", {}).get("entries", [])
            if bootstrap_entries:
                validate_nvs_block(board["nvs"]["bootstrap"], bootstrap_entries, "bootstrap")
        except (KeyError, TypeError, ValueError) as exc:
            errors.append(f"board {board_id}: {exc}")

    if errors:
        for msg in errors:
            print(f"error: {msg}", file=sys.stderr)
        return 1

    print("esp32-flasher.json OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
