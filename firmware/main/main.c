// Holoneo firmware — stage 1: Wi-Fi, HTTP API, OTA, gestures.
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "nvs_flash.h"
#include "board.h"
#include "display.h"
#include "audio.h"
#include "gesture.h"
#include "net.h"
#include "api.h"

static const char *TAG = "holoneo";

#define C_TITLE 0x00E5FF   // cyan reads well in the reflection
#define C_TEXT  0xFFFFFF
#define C_DIM   0x808080

static QueueHandle_t s_gestures;
static int s_gesture_count;
static const char *s_last_gesture = "";

static void on_gesture(gesture_t g, void *ctx) { xQueueSend(s_gestures, &g, 0); }

static void status_extra(cJSON *j)
{
    cJSON_AddNumberToObject(j, "gestures", s_gesture_count);
    cJSON_AddStringToObject(j, "last_gesture", s_last_gesture);
}

static void show_info(void)
{
    char buf[160];
    display_lock();
    display_fill(0);
    display_text(16, 12, "Holoneo", C_TITLE, 2);
    if (net_state() == NET_CONNECTED) {
        snprintf(buf, sizeof buf, "%s.local\n%s\n\ntoken: %s", NET_HOSTNAME, net_ip(), net_token());
    } else if (net_state() == NET_SETUP_AP) {
        snprintf(buf, sizeof buf, "Setup: join Wi-Fi\n%s\npassword: %s\nthen http://192.168.4.1", NET_SETUP_SSID, net_token());
    } else {
        snprintf(buf, sizeof buf, "connecting to Wi-Fi...");
    }
    display_text(16, 80, buf, C_TEXT, 1);
    display_text(16, 240 - 28, esp_app_get_description()->version, C_DIM, 1);
    display_unlock();
    display_flush();
}

static void nvs_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // Only erase when NVS is unusable — erasing loses the saved Wi-Fi network and API token.
        ESP_LOGE(TAG, "NVS unusable (%s), erasing", esp_err_to_name(err));
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

void app_main(void)
{
    ESP_LOGI(TAG, "Holoneo firmware %s", esp_app_get_description()->version);
    nvs_init();
    ESP_ERROR_CHECK(display_init());
    display_text_centered("Holoneo", C_TITLE, 3);
    display_flush();

    if (audio_init() != ESP_OK) ESP_LOGE(TAG, "audio init failed");
    s_gestures = xQueueCreate(8, sizeof(gesture_t));
    if (gesture_start(board_i2c(), on_gesture, NULL) != ESP_OK) ESP_LOGE(TAG, "gesture sensor unavailable");

    ESP_ERROR_CHECK(net_start());
    ESP_ERROR_CHECK(api_start(status_extra));
    ESP_LOGW(TAG, "API token: %s", net_token());
    show_info();
    net_state_t st = net_wait(30000);
    show_info();
    if (st == NET_CONNECTED) {
        esp_ota_mark_app_valid_cancel_rollback();   // this build works: keep it after an OTA update
    }

    // Main loop: forward gestures as events; HOVER shows the info screen for 8 s.
    bool info_shown = true, restore = false;      // restore=false: the boot info screen clears to black
    int64_t info_until = esp_timer_get_time() + 30000000;   // boot info stays 30 s
    for (;;) {
        gesture_t g;
        if (xQueueReceive(s_gestures, &g, pdMS_TO_TICKS(200))) {
            s_gesture_count++;
            s_last_gesture = gesture_name(g);
            char ev[64];
            snprintf(ev, sizeof ev, "{\"type\":\"gesture\",\"gesture\":\"%s\"}", gesture_name(g));
            api_broadcast(ev);
            if (g == GESTURE_HOVER && !info_shown) {
                display_snapshot();
                restore = true;
                show_info();
                info_shown = true;
                info_until = esp_timer_get_time() + 8000000;
            }
        }
        if (info_shown && esp_timer_get_time() > info_until) {
            info_shown = false;
            if (restore) display_restore();
            else { display_fill(0); display_flush(); }
        }
    }
}
