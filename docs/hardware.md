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
- **Gesture sensor:** see below.

## Gesture sensor (EPL2219, I2C 0x41)

- Sits on the main I2C bus (17/18) at 7-bit address **0x41**. If it doesn't ACK at all (the stock
  firmware then logs a flood of I2C NACKs), check/reseat its connector — on our unit it looked seated
  but only worked after reseating.
- **Data:** registers 0x26.. hold 8 records of 3 bytes; each record is a **20-bit value** in the top
  bits of a big-endian 24-bit word. Odd records are channel B, even records channel A (record 0 is an
  older A sample). Reading 0x26..0x2b gives the newest A and B. New samples arrive ~every 14 ms.
  Baseline ≈ 22 000–25 000, a hand in front saturates at 1 048 575.
- **Start-up** (recovered by comparing a cold sensor with one configured by the stock firmware):
  1. write regs 0x00..0x0f = `0a 14 15 14 13 20 20 10 00 00 ff ff 00 00 ff ff`
  2. write regs 0x24..0x25 = `17 6c` (without this it measures but barely reacts — LED drive/gain?)
  3. write reg 0x11 = `02` (power-up value `03`; bit 0 looks like stop/power-down)
  4. repeat steps 1–3 once more: starting the sensor from standby resets 0x24/0x25, so the
     second pass (applied while running) is what makes it work after a cold power-up.
- **What the two channels can tell:** both channels see nearly the same thing and B always trails
  A by ~18 ms (sampling order), so **left vs right can't be distinguished**. Robust gestures are
  *wave* (short burst, any direction) and *hover* (≥ 450 ms, saturated). *Bottom → top* shows a
  stronger A than B with an early peak and is usable experimentally.
  Implementation: `firmware/components/gesture/`.
