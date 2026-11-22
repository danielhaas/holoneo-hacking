# Analysing the stock firmware

The stock app is an ESP-IDF v5.4 / ESP-ADF project named `float_display`, built around a
HoloCubic-style `AppController`. Its boot log already reveals a lot (chip, PSRAM, partitions,
SD card, codecs, Wi-Fi). The pin numbers needed for custom firmware were recovered by disassembly:

1. **Extract the app** from a flash backup (factory partition at 0x10000, 3 MB):
   ```
   dd if=holoneo_backup.bin of=app.bin bs=4096 skip=16 count=768
   python3 tools/re/split_image.py app.bin re/
   ```
2. **Disassemble** the code segments with Espressif's toolchain (e.g. the `espressif/idf:v5.4` docker image):
   ```
   xtensa-esp32s3-elf-objdump -D -b binary -m xtensa --adjust-vma=0x42000020 re/seg_42000020.bin > re/irom.S
   xtensa-esp32s3-elf-objdump -D -b binary -m xtensa --adjust-vma=0x40378000 re/seg_40378000.bin > re/iram0.S
   xtensa-esp32s3-elf-objdump -D -b binary -m xtensa --adjust-vma=0x4037ac40 re/seg_4037ac40.bin > re/iram1.S
   ```
3. **Find the code behind log messages.** Xtensa loads string addresses through `l32r` literal pools,
   so: find the string → find the 4-byte literal holding its address → find the `l32r` that loads it.
   `tools/re/xref.py "Lcd gpio init!" "get_i2s_pins" ...` does exactly that.
4. **Read the pin values** next to those references (`tools/re/show.py <start> <end>`):
   - `audio_board_lcd_init`: `spi_bus_config_t` built on the stack (mosi 14, sclk 21, max transfer 0x25800)
     and `esp_lcd_panel_io_spi_config_t` (cs 47, dc 48, 60 MHz).
   - `get_i2c_pins` / `get_i2s_pins` in ESP-ADF's `board_pins_config.c`: plain `movi` + `s32i` stores.
   - Tiny getters (`entry; movi.n a2, N; retw.n`) return the LCD control pins 38 / 46 / 47 and the amp pin 39.
   - SD card init: `sdmmc_slot_config_t` with clk 15, cmd 7, d0 4, width 1.

Linear disassembly of a raw binary goes out of sync after literal pools and padding, so
instructions right after a jump can be garbled. Decode those few bytes by hand (`movi.n` is
`op0=0xC` with the 7-bit immediate split across bits 4-6 and 12-15) or restart the disassembly at the right address.
