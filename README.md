# Holoneo hacking

Notes, tools and open firmware for the **Holoneo**, a small ESP32-S3 "hologram" display
(a 240 × 320 LCD reflected in glass, with speaker, microphones, gesture sensor and microSD).

- [docs/access.md](docs/access.md) — connect over USB serial, back up / restore the flash, the stock web API and its security issues
- [docs/hardware.md](docs/hardware.md) — parts, full pin map, flash layout
- [docs/stock-firmware.md](docs/stock-firmware.md) — how the pin map was recovered from the stock firmware
- `tools/` — serial log capture and reverse-engineering helpers
- `firmware/` — our own ESP-IDF firmware (work in progress)

**Always back up your device's flash before flashing anything else** (see docs/access.md).
The backup contains your Wi-Fi password and the vendor's firmware — don't publish it.

This project is not affiliated with the maker of the Holoneo.
