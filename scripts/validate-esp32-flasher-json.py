#!/usr/bin/env python3
"""Validate esp32-flasher.json partition/NVS consistency."""

from __future__ import annotations

import csv
import json
import re
import sys
from pathlib import Path

HEX_RE = re.compile(r"^0x[0-9a-fA-F]+$")
SIZE_RE = re.compile(r"^0x[0-9a-fA-F]+$|^\d+(\.\d+)?[KkMm]?$")


def parse_hex(value: str, label: str) -> int:
    if not isinstance(value, str) or not HEX_RE.match(value):
        raise ValueError(f"{label}: expected hex string like 0x9000, got {value!r}")
    return int(value, 16)


def parse_size(value: str) -> str:
    """Normalize partition size to 0x hex string."""
    value = value.strip()
    if HEX_RE.match(value):
        return value.lower()
    m = re.match(r"^(\d+)([KkMm])?$", value)
    if not m:
        raise ValueError(f"unparseable size {value!r}")
    n = int(m.group(1))
    suffix = m.group(2)
    if suffix in ("K", "k"):
        n *= 1024
    elif suffix in ("M", "m"):
        n *= 1024 * 1024
    return f"0x{n:x}"


def parse_partitions_csv(path: Path) -> list[dict]:
    entries: list[dict] = []
    with path.open(encoding="utf-8") as fh:
        for row in csv.reader(fh):
            if not row or row[0].startswith("#"):
                continue
            name = row[0].strip()
            ptype = row[1].strip()
            subtype = row[2].strip()
            offset = row[3].strip().lower() if row[3].strip() else ""
            size = parse_size(row[4].strip())
            if offset:
                offset = offset if offset.startswith("0x") else parse_size(offset)
            entries.append(
                {
                    "name": name,
                    "type": ptype,
                    "subtype": subtype,
                    "offset": offset,
                    "size": size,
                }
            )
    # Fill blank offsets the same way ESP-IDF does (sequential after prior).
    cursor = 0
    for entry in entries:
        if entry["offset"]:
            cursor = parse_hex(entry["offset"], entry["name"]) + parse_hex(
                entry["size"], entry["name"]
            )
        else:
            entry["offset"] = f"0x{cursor:x}"
            cursor += parse_hex(entry["size"], entry["name"])
    return entries


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


def entries_equal(actual: list[dict], expected: list[dict], label: str) -> None:
    if len(actual) != len(expected):
        raise ValueError(f"{label}: {len(actual)} entries != {len(expected)} in json")
    for act, exp in zip(actual, expected):
        for key in ("name", "type", "subtype", "offset", "size"):
            if act.get(key) != exp.get(key):
                raise ValueError(
                    f"{label} {exp['name']}.{key}: csv {act.get(key)!r} != json {exp.get(key)!r}"
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

    csv_path = root / "partitions.csv"
    csv_entries = parse_partitions_csv(csv_path) if csv_path.is_file() else None

    errors: list[str] = []
    for board in boards:
        board_id = board.get("id", "<unknown>")
        try:
            full_entries = board.get("partitions_full", {}).get("entries", [])
            if not full_entries:
                raise ValueError("partitions_full.entries is empty")
            validate_nvs_block(board["nvs"]["full"], full_entries, "full")
            if csv_entries:
                entries_equal(csv_entries, full_entries, "partitions_full vs partitions.csv")

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
