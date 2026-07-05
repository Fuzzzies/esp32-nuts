# Contributing

## Partition / flasher spec changes

1. Edit [`partitions.csv`](partitions.csv) for full (IDF) firmware layout.
2. Mirror changes in [`esp32-flasher.json`](esp32-flasher.json) `partitions_full`.
3. Update [`docs/FLASH_LAYOUT.md`](docs/FLASH_LAYOUT.md).
4. Run validators:

```sh
python scripts/validate-esp32-flasher-json.py
```

For bootstrap (Arduino), after changing `standalone.ino` board/partition options:

```sh
# compile, then:
python scripts/extract-partitions-from-bin.py path/to/*.ino.partitions.bin --check-bootstrap
```

## CI artifacts

| Job | Artifact |
|-----|----------|
| test-standalone | `esp32-nuts-bootstrap-<sha>.factory.bin` |
| usb | `esp32-nuts-usb-<sha>.bin` |
| emulator | dev-only, not in releases |

## Releases

Push a `v*` tag to trigger the release job (bootstrap + USB only). Web flasher
Pages deploy runs on `release: published`.

Do not mix bootstrap and full pipeline binaries without documenting migration in
[docs/NVS.md](docs/NVS.md).
