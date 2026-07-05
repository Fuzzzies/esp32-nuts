# NVS (WiFi credentials)

WiFi credentials live in the **nvs** data partition. Offset and size differ
between the full and bootstrap pipelines — see [FLASH_LAYOUT.md](FLASH_LAYOUT.md).

Validation: `python scripts/validate-esp32-flasher-json.py` checks that
`nvs.full` / `nvs.bootstrap` in `esp32-flasher.json` match the corresponding
`partitions_*` nvs row.

## Profiles in `esp32-flasher.json`

### `esp32_nuts` — full ESP-IDF USB firmware

| Field | Value |
|-------|-------|
| Partition | nvs @ 0x9000, size 0x6000 |
| Namespace | `espnuts` |
| Keys | `wifi_ssid`, `wifi_pass` (strings) |

Implemented in [`main/wifi_mgr.c`](../main/wifi_mgr.c). The web setup portal at
`http://192.168.4.1/setup` (or `http://esp32-nuts.local/setup` on STA) writes
these keys.

Menuconfig defaults (`CONFIG_ESPNUTS_WIFI_*`) are used only when NVS is empty.

### `esp32_nuts_bootstrap` — Arduino bootstrap (`standalone.ino`)

| Field | Value |
|-------|-------|
| Partition | nvs @ 0x9000, size 0x5000 |
| Namespace | `wifi` |
| Keys | `ssid`, `password` (strings) |

Implemented via Arduino `Preferences` in
[`standalone.ino`](../standalone.ino). Setup AP: **`esp32-nuts-setup`**.

## Keys are not interchangeable

Bootstrap stores `wifi.ssid` / `wifi.password`. Full firmware reads
`espnuts.wifi_ssid` / `espnuts.wifi_pass`. Flashing full firmware **on top of**
bootstrap without erasing NVS will **not** carry WiFi credentials over.

After switching pipelines:

- Re-run WiFi setup (join `esp32-nuts-setup`, open `/setup`), or
- Use esp32-flasher / `nvs_partition_gen` to inject the correct profile.

## Emulator (dev-only)

The emulator sketch uses namespace `espnuts` with keys `ssid` and `pass` —
a third layout used only for manual Arduino IDE testing. It is **not** declared
in `esp32-flasher.json`.
