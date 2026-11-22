// File storage: internal flash (LittleFS, partition "storage", ~10 MB) at /data and the
// microSD card (FAT) at /sd. The SD card is optional.
#pragma once
#include <stdbool.h>
#include "esp_err.h"

esp_err_t storage_init(void);
bool storage_sd_present(void);
// Bytes used/total on /data.
void storage_data_usage(size_t *used, size_t *total);
