# 🥜 esp32-nuts

Tiny WiFi UPS monitor: an **ESP32-S2FN4R2** board plugged into the USB port of a
CyberPower (or any HID Power Device class) UPS via a **USB OTG** cable. It reads
the same numbers PowerPanel shows — load %, watts, battery charge, runtime,
input/output voltage — and serves them over WiFi three ways:

- **Web dashboard** — `http://esp32-nuts.local/`
- **JSON API** — `GET /api/status`
- **NUT server** on TCP **3493** — point Home Assistant, Synology DSM, or
  `upsc ups@esp32-nuts.local` straight at it (that's the "nuts" in the name:
  [Network UPS Tools](https://networkupstools.org/))

## Firmware images

| | What | How you flash it |
|---|---|---|
| **Full (USB)** | ESP-IDF project in [`main/`](main/) — USB OTG host that enumerates the UPS as a HID Power Device, parses its report descriptor, and polls Feature reports: the same protocol PowerPanel and NUT's `usbhid-ups` use. | [`esp32-flasher.json`](esp32-flasher.json) / Fuzzzies esp32-flasher · CI artifact `esp32-nuts-usb-*.bin` |
| **Bootstrap** | [`standalone.ino`](standalone.ino) — WiFi provisioning only (setup AP, no USB host). Source of truth for the Arduino bootstrap pipeline. | Same flasher spec · CI artifact `esp32-nuts-bootstrap-*.factory.bin` (when published) |
| **Emulator** *(dev only)* | [`emulator/esp32-nuts-emulator/`](emulator/esp32-nuts-emulator/) — fake UPS data for integration testing. **Not** in `esp32-flasher.json`. | Arduino IDE, manual upload |

## Flashing (esp32-flasher)

Primary flashing path: **[`esp32-flasher.json`](esp32-flasher.json)** at the repo
root (Fuzzzies [esp32-flasher](https://github.com/Fuzzzies/esp32-flasher)
standard). It defines two pipelines — **do not mix** without a full reflash:

- **Full** — merged ESP-IDF image `esp32-nuts-usb-*.bin` at offset `0x0`
  (`partitions.csv` layout)
- **Bootstrap** — Arduino `standalone.ino` merged factory image
  `esp32-nuts-bootstrap-*.factory.bin` (segments at `0x0` / `0x8000` / `0x10000`)

Details: [docs/FLASH_LAYOUT.md](docs/FLASH_LAYOUT.md) · WiFi NVS keys:
[docs/NVS.md](docs/NVS.md) · Bootstrap flow: [docs/BOOTSTRAP.md](docs/BOOTSTRAP.md)

Board: **ESP32-S2**, setup AP **`esp32-nuts-setup`**. Enter the bootloader:
**hold 0/BOOT, tap RST, release 0**, then flash. Tap RST when done.

### Manual esptool (full firmware)

```sh
pip install esptool
esptool.py --chip esp32s2 --port COM5 write_flash 0x0 esp32-nuts-usb-<sha>.bin
```

Download `esp32-nuts-usb-*.bin` and `esp32-nuts-bootstrap-*.factory.bin` from
CI workflow artifacts or GitHub Releases.

Full firmware uses NVS namespace `espnuts` (`wifi_ssid` / `wifi_pass`). Bootstrap
uses `wifi` (`ssid` / `password`). Full firmware **migrates** bootstrap credentials
when NVS is not erased — see [docs/NVS.md](docs/NVS.md).

### Web flasher (browser)

On release, GitHub Pages hosts [docs/flasher/](docs/flasher/) (ESP Web Tools).
Enable Pages source = GitHub Actions in repo settings.

### Emulator (dev only)

Not part of the flasher spec. Paste
[`emulator/esp32-nuts-emulator/esp32-nuts-emulator.ino`](emulator/esp32-nuts-emulator/esp32-nuts-emulator.ino)
into the Arduino IDE, board **ESP32S2 Dev Module**, USB CDC On Boot **Enabled**,
PSRAM **Enabled**. Serves simulated UPS data including a dashboard power-failure
button (`GET /api/sim?ac=0|1`).

## Hardware notes (read before plugging into the UPS)

- **Board**: ESP32-S2FN4R2 — 4 MB flash + 2 MB PSRAM in-package, native USB OTG
  on GPIO19/20, WiFi only.
- **VBUS in host mode**: as a USB host the ESP must supply 5 V to the UPS's
  USB-B port (the UPS won't enumerate without VBUS even though it's
  self-powered). Many S2 dev boards route the USB connector's VBUS through a
  diode *into* the board, so they can't source power out of the port. If the
  UPS never enumerates: power the board from its 5V/GND pins (a phone charger
  plugged into the UPS works nicely) and feed 5 V to the OTG cable's VBUS line.
- **Cabling**: USB-C (board, via OTG adapter) → USB-A → USB-B cable → UPS.

## Home Assistant

Settings → Devices & Services → Add → **Network UPS Tools (NUT)** →
host `esp32-nuts.local`, port `3493`, no auth. The UPS shows up as `ups` with
`ups.status`, `ups.load`, `ups.realpower`, `battery.charge`, `battery.runtime`,
`input.voltage`, `output.voltage`.

## Building the real firmware locally

```sh
# ESP-IDF v5.3.x
idf.py set-target esp32s2
idf.py build flash monitor
```

Credentials can be baked in for local builds: `idf.py menuconfig` → *esp32-nuts*.

## CI

[`.github/workflows/build.yml`](.github/workflows/build.yml):

- **test-standalone** — validates `esp32-flasher.json`, verifies bootstrap
  partitions, compiles `standalone.ino`, uploads `esp32-nuts-bootstrap-*.factory.bin`
- **emulator** job — compiles the dev emulator sketch (CI artifact only)
- **usb** job — builds ESP-IDF firmware, uploads `esp32-nuts-usb-<sha>.bin`
- **release** job — `v*` tags attach bootstrap + USB binaries (not emulator)
- **pages** job — on release, deploys web flasher to GitHub Pages

## Status / roadmap

- [x] Emulator sketch (paste into Arduino IDE, verify the whole network stack today)
- [x] Web dashboard, JSON API, minimal NUT server
- [x] WiFi provisioning portal
- [x] esp32-flasher.json dual-pipeline spec + bootstrap CI artifacts
- [x] Bootstrap → full NVS migration helper
- [x] OTA partition layout + HTTPS OTA (menuconfig URL)
- [x] ESP Web Tools page ([docs/flasher/](docs/flasher/))
- [ ] Real USB firmware validated against actual CyberPower hardware —
  see [docs/HARDWARE.md](docs/HARDWARE.md); HID fixups are heuristic until tested
- [ ] Fuzzzies esp32-flasher tool registration (external repo)
