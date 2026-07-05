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
| CI artifact | `esp32-nuts-usb-<sha>.bin` (single merged image) |
| Flash method | `idf_merged` at offset `0x0` |
| NVS profile | `esp32_nuts` — see [NVS.md](NVS.md) |
| OTA | Dual `ota_0` / `ota_1` slots — see below |

### `partitions_full` (from `partitions.csv`)

| Name | Type | Offset | Size |
|------|------|--------|------|
| nvs | data/nvs | 0x9000 | 0x6000 (24 KiB) |
| phy_init | data/phy | 0xf000 | 0x1000 (4 KiB) |
| ota_0 | app/ota_0 | 0x10000 | 0x1e0000 (~1.88 MiB) |
| ota_1 | app/ota_1 | 0x1f0000 | 0x1e0000 (~1.88 MiB) |
| otadata | data/ota | 0x3e0000 | 0x2000 (8 KiB) |

```sh
esptool.py --chip esp32s2 write_flash 0x0 esp32-nuts-usb-<sha>.bin
```

**Migration:** devices on the old single `factory` 3 MiB layout require a full
reflash when moving to OTA slots.

OTA URL (optional): `idf.py menuconfig` → *esp32-nuts* → **OTA firmware URL**.

## Bootstrap firmware (Arduino)

| Item | Value |
|------|-------|
| Source | [`standalone.ino`](../standalone.ino) (bootstrap source of truth) |
| FQBN | `esp32:esp32:esp32s2:CDCOnBoot=cdc,PSRAM=enabled` |
| Partition table | Arduino-ESP32 **default** scheme for ESP32-S2 → `partitions_bootstrap` in JSON |
| CI artifact | `esp32-nuts-bootstrap-<sha>.factory.bin` |
| Flash method | `arduino_merge_bin` segments at 0x0, 0x8000, 0x10000 |
| NVS profile | `esp32_nuts_bootstrap` — see [NVS.md](NVS.md) |

### `partitions_bootstrap`

Verified in CI against the compiled `*.ino.partitions.bin` (Arduino core default
CSV). See [`scripts/extract-partitions-from-bin.py`](../scripts/extract-partitions-from-bin.py).

If you change the sketch partition scheme, recompile, update JSON, and re-run the
validator.

## Pipeline comparison

```
Full (IDF)                         Bootstrap (Arduino)
─────────────────────────          ─────────────────────────
partitions.csv (OTA slots)         Arduino default CSV
espnuts.wifi_*  NVS keys           wifi.ssid / wifi.password
esp32-nuts-usb-*.bin               esp32-nuts-bootstrap-*.factory.bin
USB host + real UPS                WiFi provisioning only
```

## Dev-only: emulator

[`emulator/esp32-nuts-emulator/`](../emulator/esp32-nuts-emulator/) is **not**
part of `esp32-flasher.json`. Flash manually from the Arduino IDE.

## Browser flash

[`docs/flasher/`](../flasher/) — ESP Web Tools page deployed to GitHub Pages on
release (see [`.github/workflows/pages.yml`](../.github/workflows/pages.yml)).

## Switching pipelines

1. Erase flash (`esptool.py erase_flash`) or use the flasher’s full reflash.
2. Flash the target pipeline’s release asset.
3. Provision WiFi using the correct NVS profile for that pipeline.

Full firmware can **migrate** bootstrap credentials automatically — see [NVS.md](NVS.md).
