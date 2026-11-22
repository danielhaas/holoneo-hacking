# Holoneo hacking

Notes, tools and open firmware for the **Holoneo**, a small ESP32-S3 "hologram" display
(a 240 × 320 LCD reflected in glass, with speaker, microphones, gesture sensor and microSD).

- [docs/access.md](docs/access.md) — connect over USB serial, back up / restore the flash, the stock web API and its security issues
- [docs/hardware.md](docs/hardware.md) — parts, full pin map, flash layout
- [docs/stock-firmware.md](docs/stock-firmware.md) — how the pin map was recovered from the stock firmware
- [docs/api.md](docs/api.md) — HTTP API of the custom firmware (text, images, audio, mic, gestures, OTA)
- `tools/` — build/flash scripts, serial log capture, event client, reverse-engineering helpers
- `firmware/` — our own ESP-IDF firmware

## Custom firmware

Works: display (320×240 as seen in the reflection), speaker, microphones, gesture sensor
(wave / hover), Wi-Fi with mDNS (`holoneo.local`), token-protected HTTP API, WebSocket events,
over-the-air updates with automatic rollback. No cloud calls, no open access point.

```sh
tools/build.sh                 # builds in the espressif/idf:v5.4 docker image
pip install esptool
tools/flash.sh                 # first time, over USB (back up the stock flash first!)
```

After that, update over Wi-Fi: see `POST /ota` in [docs/api.md](docs/api.md).
The firmware reuses the Wi-Fi network the stock firmware had saved.

**Always back up your device's flash before flashing anything else** (see docs/access.md).
The backup contains your Wi-Fi password and the vendor's firmware — don't publish it.

This project is not affiliated with the maker of the Holoneo.
