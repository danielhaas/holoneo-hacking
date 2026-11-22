idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS "."
                       REQUIRES board gesture esp_lcd esp_driver_spi esp_driver_i2c esp_driver_i2s
                                esp_driver_gpio esp_driver_sdmmc sdmmc fatfs esp_psram esp_timer)
