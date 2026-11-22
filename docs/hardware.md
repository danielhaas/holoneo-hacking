# Holoneo hardware

Everything here was recovered from the device itself: the boot log on the USB serial port and a
disassembly of the stock firmware (ESP-ADF board name `ESP32_S3_FLOAT_DISPLAY_V1`).
No case was opened.

## Main parts

| Part | Details |
|---|---|
| SoC | ESP32-S3 (QFN56, rev v0.2), dual core 240 MHz, Wi-Fi + BLE 5 |
| PSRAM | 8 MB octal (AP Memory, 3.3 V) |
| Flash | 16 MB (the stock image header only declares 8 MB) |
| USB | CH340 USB-serial bridge (`1a86:7523`), DTR/RTS wired for ESP auto-reset |
| Display | ST7789 SPI LCD, 240 × 320, RGB565. It's reflected in glass, which makes the "hologram" |
| Speaker | ES8311 codec + amplifier |
| Microphones | ES7210 4-channel ADC, 3 mics used, TDM |
| Gesture sensor | Eminent EPL2219 (I2C) — gestures: forward, back, left, right, down, shake |
| Storage | microSD slot (SDMMC, 1-bit) |

## Pin map

| Function | GPIO | Notes |
|---|---|---|
| LCD MOSI / SCLK | 14 / 21 | SPI2_HOST, no MISO, stock clock 60 MHz |
| LCD CS / DC | 47 / 48 | no reset line |
| LCD backlight | 38 | high = on (confirmed) |
| LCD aux | 46 | driven high at init (power or reset) |
| I2C SDA / SCL | 17 / 18 | ES8311, ES7210, EPL2219 share the bus |
| I2S0 — speaker (ES8311) | MCLK 11, BCLK 2, WS 1, DOUT 8 | |
| Amplifier enable | 39 | |
| I2S1 — mics (ES7210) | MCLK 16, BCLK 9, WS 45, DIN 10 | TDM |
| microSD (SDMMC 1-bit) | CLK 15, CMD 7, D0 4 | 40 MHz, internal pull-ups |

The stock SPI bus is set up with a maximum transfer of 153 600 bytes = 240 × 320 × 2, i.e. one full frame.

## Flash layout (stock)

| Name | Offset | Size | Contents |
|---|---|---|---|
| nvs | 0x9000 | 24 KB | Wi-Fi credentials (plain text!), settings |
| phy_init | 0xF000 | 4 KB | RF calibration |
| factory | 0x10000 | 3 MB | the application (no OTA slots) |
| storage | 0x310000 | 1 MB | SPIFFS, contains only the setup web page `index.html` |

The upper 12 MB of flash are unused. Flash encryption and secure boot are **off**, so the flash can be
read and rewritten over USB with `esptool`.

## Confirmed on the device (custom firmware, stage 0)

- **Viewing orientation:** the hologram shows the panel in landscape, 320 × 240.
  With esp_lcd: `esp_lcd_panel_swap_xy(panel, true)` + `esp_lcd_panel_mirror(panel, true, false)`,
  colour inversion on (IPS), RGB order. Then logical (0,0) is the viewer's top-left.
- **Speaker:** ES8311 at I2C 0x18 via `esp_codec_dev`, amp enable GPIO39 — works.
- **Microphones:** ES7210 at I2C 0x40, 4-slot TDM at 16 kHz; signal on slots 0 and 2, slots 1 and 3 silent.
- **microSD:** mounts at 40 MHz in 1-bit mode.
- **Gesture sensor:** the stock driver talks to an EPL2219 at 7-bit address 0x41 on the same bus
  (100 kHz), but it does not ACK under our firmware — open question (power/enable path not found yet).
