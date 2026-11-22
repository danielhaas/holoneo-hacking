#!/usr/bin/env bash
# Flash the built firmware over USB serial (needs esptool on PATH and access to the port).
set -euo pipefail
PORT=${PORT:-/dev/ttyUSB0}
B="$(dirname "$0")/../firmware/build"
cd "$B"
esptool -p "$PORT" -b 921600 --before default-reset --after hard-reset write-flash \
  --flash-mode dio --flash-freq 80m --flash-size 16MB \
  0x0 bootloader/bootloader.bin \
  0x8000 partition_table/partition-table.bin \
  0xf000 ota_data_initial.bin \
  0x20000 holoneo.bin
