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

| Job | Artifact | When |
|-----|----------|------|
| test-standalone | `esp32-nuts-bootstrap-<sha>.factory.bin` | `v*` tags only (1-day retention) |
| usb | `esp32-nuts-usb-<sha>.bin` | `v*` tags only (1-day retention) |
| emulator | *(none — compile-only)* | every run |

PR and `main` pushes still build and test; binaries are uploaded only on version tags for releases.

## Releases

Push a `v*` tag to trigger the release job (bootstrap + USB only). Web flasher
Pages deploy runs on `release: published`.

Do not mix bootstrap and full pipeline binaries without documenting migration in
[docs/NVS.md](docs/NVS.md).
