# Bootstrap firmware

**Bootstrap** = [`standalone.ino`](../standalone.ino) built with the FQBN in
`esp32-flasher.json`:

```
esp32:esp32:esp32s2:CDCOnBoot=cdc,PSRAM=enabled
```

It is the **source of truth** for the Arduino bootstrap pipeline: WiFi
provisioning only, no USB host stack, no UPS polling.

## When to use bootstrap

- First-time bring-up before the full USB firmware is available or trusted.
- Browser flashing via [Fuzzzies esp32-flasher](https://github.com/Fuzzzies/esp32-flasher)
  when the bootstrap release asset is published.
- Verifying WiFi / setup AP behavior without attaching a UPS.

## Provisioning flow

1. Flash `esp32-nuts-bootstrap-*.factory.bin` (or compile/upload — see below).
2. If no credentials are baked in, join open AP **`esp32-nuts-setup`**.
3. Open `http://192.168.4.1/setup` and save SSID/password.
4. Device reboots onto your LAN as `esp32-nuts.local` (bootstrap status page on `/`).

Credentials are stored under NVS profile **`esp32_nuts_bootstrap`**
(`wifi.ssid`, `wifi.password`) — see [NVS.md](NVS.md).

## Moving to full firmware

Bootstrap and full firmware are **different pipelines** (partition table, app
layout, NVS keys). To run the real USB UPS monitor:

1. **Erase flash** or perform a full reflash of the IDF merged image.
2. Flash `esp32-nuts-usb-*.bin` (`idf_merged` at 0x0).
3. Provision again with the **`esp32_nuts`** NVS profile, or use menuconfig for
   local builds.

Do not expect bootstrap WiFi credentials to survive a pipeline switch **if NVS
was erased**. Full firmware migrates bootstrap keys when NVS is intact — see
[NVS.md](NVS.md).

### Compile locally

```sh
mkdir -p standalone && cp standalone.ino standalone/standalone.ino
arduino-cli compile --fqbn "esp32:esp32:esp32s2:CDCOnBoot=cdc,PSRAM=enabled" standalone
```

## CI

The `test-standalone` job in [`.github/workflows/build.yml`](../.github/workflows/build.yml)
compiles `standalone.ino` with the JSON FQBN and runs
`scripts/validate-esp32-flasher-json.py`. Bootstrap release binaries are
produced separately (not in this spec PR’s release job).

## Not bootstrap: emulator

[`emulator/esp32-nuts-emulator/`](../emulator/esp32-nuts-emulator/) is a
**dev-only** sketch with simulated UPS data. It is not referenced in
`esp32-flasher.json` and is not a substitute for bootstrap or full firmware.
