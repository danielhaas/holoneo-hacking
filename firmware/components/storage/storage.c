#include "esp_log.h"
#include "esp_littlefs.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "board.h"
#include "storage.h"

static const char *TAG = "storage";
static bool s_sd;

esp_err_t storage_init(void)
{
    esp_vfs_littlefs_conf_t lfs = { .base_path = "/data", .partition_label = "storage",
                                    .format_if_mount_failed = true, .dont_mount = false };
    esp_err_t err = esp_vfs_littlefs_register(&lfs);
    if (err != ESP_OK) ESP_LOGE(TAG, "/data mount failed: %s", esp_err_to_name(err));
    else { size_t u, t; storage_data_usage(&u, &t); ESP_LOGI(TAG, "/data: %u of %u KB used", (unsigned)(u / 1024), (unsigned)(t / 1024)); }

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 1;
    slot.clk = BOARD_SD_CLK; slot.cmd = BOARD_SD_CMD; slot.d0 = BOARD_SD_D0;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
    esp_vfs_fat_sdmmc_mount_config_t mc = { .format_if_mount_failed = false, .max_files = 4, .allocation_unit_size = 16 * 1024 };
    sdmmc_card_t *card;
    if (esp_vfs_fat_sdmmc_mount("/sd", &host, &slot, &mc, &card) == ESP_OK) {
        s_sd = true;
        ESP_LOGI(TAG, "/sd: %llu MB card", (unsigned long long)card->csd.capacity * card->csd.sector_size / (1024 * 1024));
    } else {
        ESP_LOGW(TAG, "no SD card");
    }
    return err;
}

bool storage_sd_present(void) { return s_sd; }

void storage_data_usage(size_t *used, size_t *total)
{
    *used = *total = 0;
    esp_littlefs_info("storage", total, used);
}
