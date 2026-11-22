#!/usr/bin/env bash
# Build the firmware in Espressif's ESP-IDF v5.4 container. Extra args go to idf.py.
set -euo pipefail
cd "$(dirname "$0")/../firmware"
docker run --rm -u "$(id -u):$(id -g)" -e HOME=/tmp -v "$PWD":/project -w /project \
  espressif/idf:v5.4 bash -c "idf.py ${*:-build} 2>&1 | grep -vE '^(Checking|Python|\"python3\"|Activating|\* |Done!|Go to|  idf.py build|$)'"
