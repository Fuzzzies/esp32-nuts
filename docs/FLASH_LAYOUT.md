# Flash layout

esp32-nuts ships two **independent** flash pipelines. They use different partition
tables, NVS key layouts, and release artifact names. Do not mix images from one
pipeline onto a device flashed with the other without a **full chip erase and
reflash** of every segment.

The machine-readable spec is [`esp32-flasher.json`](../esp32-flasher.json) at the
repo root (Fuzzzies [esp32-flasher](https://github.com/Fuzzzies/esp32-flasher)
standard).

## Full firmware (ESP-IDF USB host)

| Item | Value |
|------|-------|
| Source | [`main/`](../main/) ESP-IDF project |
| Partition table | [`partitions.csv`](../partitions.csv) → `partitions_full` in JSON |
| CI artifact | `esp32-nuts-usb-*.bin` (single merged image) |
| Flash method | `idf_merged` at offset `0x0` |
| NVS profile | `esp32_nuts` — see [NVS.md](NVS.md) |

### `partitions_full` (from `partitions.csv`)

| Name | Type | Offset | Size |
|------|------|--------|------|
| nvs | data/nvs | 0x9000 | 0x6000 (24 KiB) |
| phy_init | data/phy | 0xf000 | 0x1000 (4 KiB) |
| factory | app/factory | 0x10000 | 0x300000 (3 MiB) |

```sh
esptool.py --chip esp32s2 write_flash 0x0 esp32-nuts-usb.bin
```

## Bootstrap firmware (Arduino)

| Item | Value |
|------|-------|
| Source | [`standalone.ino`](../standalone.ino) (bootstrap source of truth) |
| FQBN | `esp32:esp32:esp32s2:CDCOnBoot=cdc,PSRAM=enabled` |
| Partition table | Arduino-ESP32 **default** scheme for ESP32-S2 → `partitions_bootstrap` in JSON |
| CI artifact | `esp32-nuts-bootstrap-*.factory.bin` |
| Flash method | `arduino_merge_bin` segments at 0x0, 0x8000, 0x10000 |
| NVS profile | `esp32_nuts_bootstrap` — see [NVS.md](NVS.md) |

### `partitions_bootstrap`

Filled from the first `standalone.ino` compile (Arduino core default partition
CSV). Current values match `tools/partitions/default.csv` in arduino-esp32:

| Name | Type | Offset | Size |
|------|------|--------|------|
| nvs | data/nvs | 0x9000 | 0x5000 |
| otadata | data/ota | 0xe000 | 0x2000 |
| app0 | app/ota_0 | 0x10000 | 0x140000 |
| app1 | app/ota_1 | 0x150000 | 0x140000 |
| spiffs | data/spiffs | 0x290000 | 0x160000 |
| coredump | data/coredump | 0x3f0000 | 0x10000 |

If you change the sketch partition scheme (custom `partitions.csv` next to the
sketch, or a different board option), recompile, update `partitions_bootstrap`
in `esp32-flasher.json`, and run `scripts/validate-esp32-flasher-json.py`.

## Pipeline comparison

```
Full (IDF)                         Bootstrap (Arduino)
─────────────────────────          ─────────────────────────
partitions.csv                     Arduino default CSV
3 MiB factory app                  Dual OTA slots + SPIFFS
espnuts.wifi_*  NVS keys           wifi.ssid / wifi.password
esp32-nuts-usb-*.bin               esp32-nuts-bootstrap-*.factory.bin
USB host + real UPS                WiFi provisioning only
```

## Dev-only: emulator

[`emulator/esp32-nuts-emulator/`](../emulator/esp32-nuts-emulator/) is **not**
part of `esp32-flasher.json`. It is an Arduino sketch for local integration
testing with simulated UPS data. Flash it manually from the Arduino IDE; do not
assume its partition table or NVS layout matches either pipeline above.

## Switching pipelines

1. Erase flash (`esptool.py erase_flash`) or use the flasher’s full reflash.
2. Flash the target pipeline’s release asset.
3. Provision WiFi using the correct NVS profile for that pipeline.

See [BOOTSTRAP.md](BOOTSTRAP.md) for when to use bootstrap vs full firmware.
