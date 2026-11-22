// Holoneo (ESP32-S3 "float display") pin map — recovered from the stock firmware.
// See docs/hardware.md.
#pragma once

// LCD: ST7789, 240x320 RGB565 on SPI2
#define BOARD_LCD_HOST      SPI2_HOST
#define BOARD_LCD_MOSI      14
#define BOARD_LCD_SCLK      21
#define BOARD_LCD_CS        47
#define BOARD_LCD_DC        48
#define BOARD_LCD_RST       (-1)
#define BOARD_LCD_BL        38
#define BOARD_LCD_AUX       46   // driven high by stock firmware at LCD init
#define BOARD_LCD_W         240
#define BOARD_LCD_H         320

// I2C bus shared by ES8311, ES7210 and the EPL2219 gesture sensor
#define BOARD_I2C_SDA       17
#define BOARD_I2C_SCL       18

// I2S0: speaker (ES8311)
#define BOARD_SPK_MCLK      11
#define BOARD_SPK_BCLK      2
#define BOARD_SPK_WS        1
#define BOARD_SPK_DOUT      8
#define BOARD_PA_EN         39

// I2S1: microphones (ES7210, TDM)
#define BOARD_MIC_MCLK      16
#define BOARD_MIC_BCLK      9
#define BOARD_MIC_WS        45
#define BOARD_MIC_DIN       10

// microSD, SDMMC 1-bit
#define BOARD_SD_CLK        15
#define BOARD_SD_CMD        7
#define BOARD_SD_D0         4
