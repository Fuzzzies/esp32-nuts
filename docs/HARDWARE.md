# Hardware validation

On-device checklist for the **full USB firmware** (`main/ups_usb.c`) against a
real CyberPower (or other HID Power Device class) UPS. The emulator cannot
substitute for these tests.

## Prerequisites

- ESP32-S2FN4R2 dev board (4 MB flash, 2 MB PSRAM)
- USB OTG adapter: board USB-C → USB-A → UPS USB-B
- **VBUS**: many S2 boards cannot source 5 V out the USB port — power the board
  from 5V/GND pins if the UPS never enumerates (see [README](../README.md))
- Serial monitor on UART0 (USB is used by the host stack in full firmware)

## Flash and provision

1. Full reflash: `esptool.py --chip esp32s2 write_flash 0x0 esp32-nuts-usb-<sha>.bin`
2. Provision WiFi via `esp32-nuts-setup` → `http://192.168.4.1/setup`, or migrate
   from bootstrap (see [NVS.md](NVS.md))

## Validation checklist

| Step | Pass criteria | Notes |
|------|---------------|-------|
| USB enumerate | Serial shows `UPS connected` | If not, check VBUS and cable |
| Descriptor parse | Log lines `usage 0084:.... -> feature/input report` | Zero usages → wrong device class |
| Feature poll | `GET /api/status` shows non-NaN fields within ~5 s | Poll interval 2 s |
| AC present | `status` contains `OL` on mains | Toggle UPS input or pull cord |
| On battery | `status` contains `OB` | |
| Load % | Within ~5% of PowerPanel / `upsc` | |
| Battery charge | 0–100, tracks PowerPanel | |
| Runtime | Within ~10% of PowerPanel (seconds) | CyberPower quirk: minutes — fixed in firmware |
| Battery voltage | 10–16 V typical | Centivolt quirk — fixed in firmware |
| Input/output V | ~120 V (region dependent) | |
| NUT | `upsc ups@esp32-nuts.local` returns vars | Port 3493 |
| Home Assistant | NUT integration discovers `ups` | |

## Known CyberPower quirks (firmware fixups)

Implemented in [`main/ups_usb.c`](../main/ups_usb.c) `normalize_readings()`:

1. **Runtime in minutes** — values under 600 are multiplied by 60 (NUT
   `usbhid-ups` heuristic).
2. **Battery voltage in centivolts** — values between 50 and 500 are divided by
   100.

Unit exponents in HID reports are handled in [`main/hid_parser.c`](../main/hid_parser.c)
(`unit_correction()` for SI scaling).

## Reporting issues

When filing a bug, include:

- UPS model and serial (from `/api/status` `mfr` / `model` / `serial`)
- Serial log from connect through first poll
- Raw `/api/status` JSON alongside PowerPanel screenshot or `upsc` output

## Status

| Environment | Status |
|-------------|--------|
| Emulator (simulated UPS) | Validated in CI + manual |
| Real CyberPower USB | **Needs community hardware confirmation** — fixups are heuristic until tested on specific models |
