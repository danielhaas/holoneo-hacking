#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_check.h"
#include "gesture.h"

static const char *TAG = "gesture";

#define EPL_ADDR        0x41
#define EPL_DATA_REG    0x26     // newest sample: A (3 bytes), B (3 bytes), 20-bit big-endian, top bits
#define TRIGGER         6000     // counts above baseline that start a burst
#define SATURATED       1000000  // 20-bit full scale is 1048575
#define HOVER_MIN_MS    450
#define END_QUIET_MS    100

// Start-up sequence, recovered by comparing a cold sensor with one configured by the stock
// firmware: after power-up reg 0x00 is 0x00 and reg 0x11 is 0x03 (standby). Writing the
// stock values to 0x00-0x0f (0x00 = 0x0a looks like the operating mode) and then
// 0x11 = 0x02 (bit 0 = power-down/stop) starts the measurements.
static const uint8_t k_cfg[0x10] = {
    0x0a, 0x14, 0x15, 0x14, 0x13, 0x20, 0x20, 0x10, 0x00, 0x00, 0xff, 0xff, 0x00, 0x00, 0xff, 0xff,
};
#define EPL_REG_MODE    0x11
#define EPL_MODE_RUN    0x02
// 0x24/0x25 read 0x17 0x6c after the stock firmware configured the sensor but 0x94 0xe0 after a
// cold power-up; without the stock values the sensor measures but barely reacts to a hand
// (likely IR LED drive / gain).
static const uint8_t k_led[3] = {0x24, 0x17, 0x6c};

static i2c_master_dev_handle_t s_dev;
static gesture_cb_t s_cb;
static void *s_ctx;

const char *gesture_name(gesture_t g)
{
    switch (g) {
    case GESTURE_WAVE:  return "WAVE";
    case GESTURE_HOVER: return "HOVER";
    case GESTURE_UP:    return "UP";
    }
    return "?";
}

static inline uint32_t u20(const uint8_t *p) { return ((uint32_t)p[0] << 16 | p[1] << 8 | p[2]) >> 4; }

static esp_err_t read_ab(uint32_t *a, uint32_t *b)
{
    uint8_t r = EPL_DATA_REG, d[6];
    esp_err_t e = i2c_master_transmit_receive(s_dev, &r, 1, d, 6, 20);
    if (e == ESP_OK) { *a = u20(d); *b = u20(d + 3); }
    return e;
}

// True if the data registers change within ~200 ms (i.e. the sensor is measuring).
static bool sensor_running(void)
{
    uint32_t a0, b0, a, b;
    if (read_ab(&a0, &b0) != ESP_OK) return false;
    for (int i = 0; i < 40; i++) {
        vTaskDelay(pdMS_TO_TICKS(5));
        if (read_ab(&a, &b) == ESP_OK && (a != a0 || b != b0)) return true;
    }
    return false;
}

static void classify(int64_t dur_ms, uint32_t pk_a, uint32_t pk_b, int64_t t_pk_a, double sum_a, double sum_b)
{
    double ratio = sum_b > 0 ? sum_a / sum_b : 0;
    gesture_t g;
    if (dur_ms >= HOVER_MIN_MS && (pk_a > SATURATED || pk_b > SATURATED)) g = GESTURE_HOVER;
    else if (ratio > 1.0 && t_pk_a < 70) g = GESTURE_UP;
    else g = GESTURE_WAVE;
    ESP_LOGI(TAG, "%s (dur %lld ms, ratio A/B %.2f, peak A %lu @%lld ms)", gesture_name(g),
             dur_ms, ratio, (unsigned long)pk_a, t_pk_a);
    if (s_cb) s_cb(g, s_ctx);
}

static void gesture_task(void *arg)
{
    float base_a = 0, base_b = 0;
    bool in_burst = false;
    int64_t t0 = 0, t_last_active = 0, t_pk_a = 0;
    uint32_t pk_a = 0, pk_b = 0;
    double sum_a = 0, sum_b = 0;
    int errors = 0;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(5));
        uint32_t a, b;
        if (read_ab(&a, &b) != ESP_OK) {
            if (++errors % 200 == 1) ESP_LOGW(TAG, "sensor read failed (%d)", errors);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        int64_t now = esp_timer_get_time() / 1000;
        if (base_a == 0) { base_a = a; base_b = b; }
        float ea = a - base_a, eb = b - base_b;
        bool active = ea > TRIGGER || eb > TRIGGER;

        if (!in_burst) {
            base_a += 0.01f * (a - base_a);   // slow baseline tracking while idle
            base_b += 0.01f * (b - base_b);
            if (!active) continue;
            in_burst = true; t0 = now; pk_a = pk_b = 0; sum_a = sum_b = 0;
        }
        if (active) t_last_active = now;
        if (ea > 0) sum_a += ea;
        if (eb > 0) sum_b += eb;
        if (a > pk_a) { pk_a = a; t_pk_a = now - t0; }
        if (b > pk_b) pk_b = b;
        if (now - t_last_active > END_QUIET_MS) {
            in_burst = false;
            classify(t_last_active - t0, pk_a, pk_b, t_pk_a, sum_a, sum_b);
        }
    }
}

esp_err_t gesture_start(i2c_master_bus_handle_t bus, gesture_cb_t cb, void *ctx)
{
    s_cb = cb; s_ctx = ctx;
    i2c_device_config_t dc = { .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = EPL_ADDR,
                               .scl_speed_hz = 400000 };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &dc, &s_dev), TAG, "add device");
    if (i2c_master_probe(bus, EPL_ADDR, 50) != ESP_OK) {
        ESP_LOGE(TAG, "EPL2219 not found at 0x%02x (check the sensor connector)", EPL_ADDR);
        return ESP_ERR_NOT_FOUND;
    }
    // Run the configuration twice: the first pass wakes the sensor from power-up standby,
    // the second re-applies everything while it is running (the case that reliably works;
    // starting the sensor appears to reset 0x24/0x25 otherwise).
    for (int pass = 0; pass < 2; pass++) {
        uint8_t buf[1 + sizeof k_cfg];
        buf[0] = 0x00;
        memcpy(buf + 1, k_cfg, sizeof k_cfg);
        ESP_RETURN_ON_ERROR(i2c_master_transmit(s_dev, buf, sizeof buf, 50), TAG, "write config");
        ESP_RETURN_ON_ERROR(i2c_master_transmit(s_dev, k_led, sizeof k_led, 50), TAG, "write led");
        uint8_t w[2] = {EPL_REG_MODE, EPL_MODE_RUN};
        ESP_RETURN_ON_ERROR(i2c_master_transmit(s_dev, w, 2, 50), TAG, "start sensor");
        vTaskDelay(pdMS_TO_TICKS(50));
        uint8_t r = 0x24, chk[2];
        if (i2c_master_transmit_receive(s_dev, &r, 1, chk, 2, 50) == ESP_OK)
            ESP_LOGI(TAG, "init pass %d: reg 0x24/0x25 = %02x %02x", pass + 1, chk[0], chk[1]);
    }
    if (!sensor_running()) { ESP_LOGE(TAG, "sensor idle after configuration"); return ESP_FAIL; }
    ESP_LOGI(TAG, "EPL2219 running");
    xTaskCreate(gesture_task, "gesture", 3072, NULL, 5, NULL);
    return ESP_OK;
}
