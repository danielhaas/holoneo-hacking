// Stage 0 — hardware bring-up probe for the Holoneo.
// Every step logs to the serial console (115200) with the tag "probe".
#include <math.h>
#include <string.h>
#include <dirent.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "driver/i2s_tdm.h"
#include "driver/sdmmc_host.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "board.h"
#include "gesture.h"
#include "freertos/queue.h"

static const char *TAG = "probe";

// ---------------------------------------------------------------- display
#define BAND_LINES 40
#define VIEW_W BOARD_LCD_H   // landscape as seen in the reflection
#define VIEW_H BOARD_LCD_W
static esp_lcd_panel_handle_t s_panel;
static SemaphoreHandle_t s_flush_done;
static uint16_t *s_band;   // DMA-capable internal RAM, BAND_LINES lines

static bool on_flush_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *e, void *ctx)
{
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_flush_done, &woken);
    return woken == pdTRUE;
}

static inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    uint16_t c = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
    return (c >> 8) | (c << 8);   // panel expects big-endian
}

static void display_init(void)
{
    gpio_config_t out = {
        .pin_bit_mask = (1ULL << BOARD_LCD_AUX) | (1ULL << BOARD_LCD_BL),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&out);
    gpio_set_level(BOARD_LCD_AUX, 1);
    gpio_set_level(BOARD_LCD_BL, 1);

    spi_bus_config_t bus = {
        .mosi_io_num = BOARD_LCD_MOSI, .miso_io_num = -1, .sclk_io_num = BOARD_LCD_SCLK,
        .quadwp_io_num = -1, .quadhd_io_num = -1,
        .max_transfer_sz = BOARD_LCD_H * BAND_LINES * 2,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(BOARD_LCD_HOST, &bus, SPI_DMA_CH_AUTO));

    s_flush_done = xSemaphoreCreateBinary();
    esp_lcd_panel_io_handle_t io;
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = BOARD_LCD_CS, .dc_gpio_num = BOARD_LCD_DC,
        .spi_mode = 0, .pclk_hz = 40 * 1000 * 1000, .trans_queue_depth = 4,
        .lcd_cmd_bits = 8, .lcd_param_bits = 8,
        .on_color_trans_done = on_flush_done,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)BOARD_LCD_HOST, &io_cfg, &io));

    esp_lcd_panel_dev_config_t pcfg = {
        .reset_gpio_num = BOARD_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io, &pcfg, &s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(s_panel, true));   // IPS panels usually need this
    // Round 1 showed the viewer sees the panel in landscape: panel y runs left->right and
    // panel x runs bottom->top. Swap axes and flip panel x so logical (0,0) is the viewer's top-left.
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(s_panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(s_panel, true, false));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));

    s_band = heap_caps_malloc(VIEW_W * BAND_LINES * 2, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    assert(s_band);
    ESP_LOGI(TAG, "display: init ok (landscape %dx%d, swap_xy, mirror_x)", VIEW_W, VIEW_H);
}

// Pixel function for the test pattern (viewer coordinates, 320x240).
static uint16_t pattern(int x, int y)
{
    // 3-px white frame around the edge -> shows offsets / clipping
    if (x < 3 || y < 3 || x >= VIEW_W - 3 || y >= VIEW_H - 3) return rgb565(255, 255, 255);
    // 30x30 yellow marker in the top-left corner -> shows orientation / mirroring
    if (x < 33 && y < 33) return rgb565(255, 255, 0);
    // five horizontal bands: red, green, blue, white, black
    static const uint8_t bands[5][3] = {{255,0,0},{0,255,0},{0,0,255},{255,255,255},{0,0,0}};
    int b = y * 5 / VIEW_H;
    return rgb565(bands[b][0], bands[b][1], bands[b][2]);
}

static void draw_test_pattern(void)
{
    for (int y0 = 0; y0 < VIEW_H; y0 += BAND_LINES) {
        for (int y = 0; y < BAND_LINES; y++)
            for (int x = 0; x < VIEW_W; x++)
                s_band[y * VIEW_W + x] = pattern(x, y0 + y);
        esp_lcd_panel_draw_bitmap(s_panel, 0, y0, VIEW_W, y0 + BAND_LINES, s_band);
        xSemaphoreTake(s_flush_done, portMAX_DELAY);
    }
    ESP_LOGI(TAG, "display: test pattern drawn (top->bottom: RED GREEN BLUE WHITE BLACK, "
                  "white frame, yellow square top-left)");
}

// ---------------------------------------------------------------- I2C
static i2c_master_bus_handle_t s_i2c;

static void i2c_scan(void)
{
    i2c_master_bus_config_t cfg = {
        .i2c_port = I2C_NUM_0, .sda_io_num = BOARD_I2C_SDA, .scl_io_num = BOARD_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&cfg, &s_i2c));
    int found = 0;
    for (int a = 0x08; a < 0x78; a++) {
        if (i2c_master_probe(s_i2c, a, 50) == ESP_OK) {
            const char *who = a == 0x18 ? "ES8311 (speaker codec)" :
                              a == 0x40 ? "ES7210 (mic ADC)" : "unknown -> gesture sensor?";
            ESP_LOGI(TAG, "i2c: device at 0x%02x  %s", a, who);
            found++;
        }
    }
    ESP_LOGI(TAG, "i2c: scan done, %d device(s)", found);
}

// ---------------------------------------------------------------- audio
static esp_codec_dev_handle_t s_spk;

static void speaker_init(void)
{
    i2s_chan_handle_t tx;
    i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    cc.auto_clear = true;
    ESP_ERROR_CHECK(i2s_new_channel(&cc, &tx, NULL));
    i2s_std_config_t std = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(16000),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = { .mclk = BOARD_SPK_MCLK, .bclk = BOARD_SPK_BCLK, .ws = BOARD_SPK_WS,
                      .dout = BOARD_SPK_DOUT, .din = I2S_GPIO_UNUSED },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx, &std));

    audio_codec_i2s_cfg_t i2s_cfg = { .port = I2S_NUM_0, .tx_handle = tx };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);
    audio_codec_i2c_cfg_t i2c_cfg = { .port = I2C_NUM_0, .addr = ES8311_CODEC_DEFAULT_ADDR, .bus_handle = s_i2c };
    const audio_codec_ctrl_if_t *ctrl = audio_codec_new_i2c_ctrl(&i2c_cfg);
    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    es8311_codec_cfg_t es = {
        .ctrl_if = ctrl, .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin = BOARD_PA_EN, .use_mclk = true,
        .hw_gain = { .pa_voltage = 5.0, .codec_dac_voltage = 3.3 },
    };
    const audio_codec_if_t *codec = es8311_codec_new(&es);
    if (!codec) { ESP_LOGE(TAG, "speaker: ES8311 init failed"); return; }
    esp_codec_dev_cfg_t dc = { .dev_type = ESP_CODEC_DEV_TYPE_OUT, .codec_if = codec, .data_if = data_if };
    esp_codec_dev_handle_t spk = esp_codec_dev_new(&dc);
    esp_codec_dev_set_out_vol(spk, 60);
    esp_codec_dev_sample_info_t fs = { .bits_per_sample = 16, .channel = 2, .sample_rate = 16000 };
    if (esp_codec_dev_open(spk, &fs) != ESP_CODEC_DEV_OK) { ESP_LOGE(TAG, "speaker: open failed"); return; }

    s_spk = spk;
    ESP_LOGI(TAG, "speaker: ES8311 ready");
}

static void beep3(void)
{
    if (!s_spk) return;
    const int n = 16000 / 4;   // 250 ms per beep
    int16_t *buf = heap_caps_malloc(n * 4, MALLOC_CAP_SPIRAM);
    const float freqs[3] = {440, 660, 880};
    ESP_LOGW(TAG, "speaker: 3 rising beeps NOW (440/660/880 Hz)");
    for (int f = 0; f < 3; f++) {
        for (int i = 0; i < n; i++) {
            float env = i < 400 ? i / 400.0f : (i > n - 400 ? (n - i) / 400.0f : 1.0f);
            int16_t v = (int16_t)(8000 * env * sinf(2 * M_PI * freqs[f] * i / 16000));
            buf[2 * i] = buf[2 * i + 1] = v;
        }
        esp_codec_dev_write(s_spk, buf, n * 4);
    }
    free(buf);
}

static void mic_test(void)
{
    i2s_chan_handle_t rx;
    i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&cc, NULL, &rx));
    i2s_tdm_config_t tdm = {
        .clk_cfg = I2S_TDM_CLK_DEFAULT_CONFIG(16000),
        .slot_cfg = I2S_TDM_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO,
                        I2S_TDM_SLOT0 | I2S_TDM_SLOT1 | I2S_TDM_SLOT2 | I2S_TDM_SLOT3),
        .gpio_cfg = { .mclk = BOARD_MIC_MCLK, .bclk = BOARD_MIC_BCLK, .ws = BOARD_MIC_WS,
                      .dout = I2S_GPIO_UNUSED, .din = BOARD_MIC_DIN },
    };
    ESP_ERROR_CHECK(i2s_channel_init_tdm_mode(rx, &tdm));

    audio_codec_i2s_cfg_t i2s_cfg = { .port = I2S_NUM_1, .rx_handle = rx };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);
    audio_codec_i2c_cfg_t i2c_cfg = { .port = I2C_NUM_0, .addr = ES7210_CODEC_DEFAULT_ADDR, .bus_handle = s_i2c };
    const audio_codec_ctrl_if_t *ctrl = audio_codec_new_i2c_ctrl(&i2c_cfg);
    es7210_codec_cfg_t es = {
        .ctrl_if = ctrl,
        .mic_selected = ES7210_SEL_MIC1 | ES7210_SEL_MIC2 | ES7210_SEL_MIC3 | ES7210_SEL_MIC4,
    };
    const audio_codec_if_t *codec = es7210_codec_new(&es);
    if (!codec) { ESP_LOGE(TAG, "mic: ES7210 init failed"); return; }
    esp_codec_dev_cfg_t dc = { .dev_type = ESP_CODEC_DEV_TYPE_IN, .codec_if = codec, .data_if = data_if };
    esp_codec_dev_handle_t mic = esp_codec_dev_new(&dc);
    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = 16, .channel = 4, .sample_rate = 16000,
        .channel_mask = ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0) | ESP_CODEC_DEV_MAKE_CHANNEL_MASK(1) |
                        ESP_CODEC_DEV_MAKE_CHANNEL_MASK(2) | ESP_CODEC_DEV_MAKE_CHANNEL_MASK(3),
    };
    if (esp_codec_dev_open(mic, &fs) != ESP_CODEC_DEV_OK) { ESP_LOGE(TAG, "mic: open failed"); return; }
    esp_codec_dev_set_in_gain(mic, 30.0);

    const int frames = 1600;   // 100 ms
    int16_t *buf = heap_caps_malloc(frames * 4 * 2, MALLOC_CAP_SPIRAM);
    ESP_LOGW(TAG, "mic: recording 4 s — quiet for 2 s, then CLAP or talk  <--");
    for (int blk = 0; blk < 40; blk++) {
        if (esp_codec_dev_read(mic, buf, frames * 4 * 2) != ESP_CODEC_DEV_OK) { ESP_LOGE(TAG, "mic: read failed"); break; }
        if (blk % 5 == 4) {   // report every 500 ms (last block)
            double acc[4] = {0};
            int peak[4] = {0};
            for (int i = 0; i < frames; i++)
                for (int c = 0; c < 4; c++) {
                    int v = buf[i * 4 + c];
                    acc[c] += (double)v * v;
                    if (abs(v) > peak[c]) peak[c] = abs(v);
                }
            ESP_LOGI(TAG, "mic t=%.1fs  rms ch0..3: %6.0f %6.0f %6.0f %6.0f   peak: %5d %5d %5d %5d",
                     (blk + 1) / 10.0, sqrt(acc[0] / frames), sqrt(acc[1] / frames),
                     sqrt(acc[2] / frames), sqrt(acc[3] / frames), peak[0], peak[1], peak[2], peak[3]);
        }
    }
    free(buf);
    esp_codec_dev_close(mic);
    ESP_LOGI(TAG, "mic: done");
}

// ---------------------------------------------------------------- SD card
static void sd_test(void)
{
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 1;
    slot.clk = BOARD_SD_CLK;
    slot.cmd = BOARD_SD_CMD;
    slot.d0 = BOARD_SD_D0;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
    esp_vfs_fat_sdmmc_mount_config_t mc = { .format_if_mount_failed = false, .max_files = 4 };
    sdmmc_card_t *card;
    esp_err_t err = esp_vfs_fat_sdmmc_mount("/sd", &host, &slot, &mc, &card);
    if (err != ESP_OK) { ESP_LOGE(TAG, "sd: mount failed: %s", esp_err_to_name(err)); return; }
    sdmmc_card_print_info(stdout, card);
    const char *dirs[] = {"/sd", "/sd/VIDEO", "/sd/PICTURE"};
    for (int d = 0; d < 3; d++) {
        DIR *dir = opendir(dirs[d]);
        if (!dir) { ESP_LOGI(TAG, "sd: %s (missing)", dirs[d]); continue; }
        ESP_LOGI(TAG, "sd: listing %s", dirs[d]);
        struct dirent *e;
        int n = 0;
        while ((e = readdir(dir)) && n++ < 40)
            ESP_LOGI(TAG, "sd:   %s%s", e->d_name, e->d_type == DT_DIR ? "/" : "");
        closedir(dir);
    }
    esp_vfs_fat_sdcard_unmount("/sd", card);
}

// ---------------------------------------------------------------- gesture sensor
// Stock driver talks to the EPL2219 at 8-bit address 0x82 (7-bit 0x41), 100 kHz, same bus.
static void gesture_probe(void)
{
    // i2c_master_probe() sends an empty write; some sensors ignore that. Do real reads instead.
    const uint32_t speeds[2] = {100000, 20000};
    const uint8_t addrs[2] = {0x41, 0x40 ^ 0x01};
    for (int si = 0; si < 2; si++) {
        i2c_master_dev_handle_t dev;
        i2c_device_config_t dc = { .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = addrs[0],
                                   .scl_speed_hz = speeds[si] };
        if (i2c_master_bus_add_device(s_i2c, &dc, &dev) != ESP_OK) continue;
        char line[128]; int o = 0, ok = 0;
        for (uint8_t r = 0; r < 0x20; r++) {
            uint8_t v = 0;
            esp_err_t e = i2c_master_transmit_receive(dev, &r, 1, &v, 1, 100);
            if (e == ESP_OK) ok++;
            o += snprintf(line + o, sizeof line - o, e == ESP_OK ? "%02x " : "-- ", v);
            if (r % 16 == 15) { ESP_LOGI(TAG, "gesture 0x41 @%luHz regs %02x: %s", (unsigned long)speeds[si], r - 15, line); o = 0; }
        }
        ESP_LOGI(TAG, "gesture 0x41 @%luHz: %d/32 reads ok", (unsigned long)speeds[si], ok);
        i2c_master_bus_rm_device(dev);
    }
    (void)addrs;
}

static void rescan(const char *when)
{
    char line[96]; int o = 0;
    for (int a = 0x08; a < 0x78; a++)
        if (i2c_master_probe(s_i2c, a, 20) == ESP_OK) o += snprintf(line + o, sizeof line - o, "0x%02x ", a);
    line[o] = 0;
    ESP_LOGI(TAG, "i2c rescan (%s): %s", when, o ? line : "none");
}

// Brute-force I2C scan over every pair of GPIOs not known to be in use, looking for the
// gesture sensor in case it is wired to other pins than the stock firmware assumes.
static void pin_hunt(void)
{
    const int cand[] = {0, 3, 5, 6, 12, 13, 19, 20, 40, 41, 42};
    const int n = sizeof cand / sizeof cand[0];
    ESP_LOGW(TAG, "pin hunt: %d candidate pins, %d pairs", n, n * (n - 1));
    for (int i = 0; i < n; i++)
        for (int j = 0; j < n; j++) {
            if (i == j) continue;
            i2c_master_bus_handle_t bus;
            i2c_master_bus_config_t cfg = {
                .i2c_port = I2C_NUM_1, .sda_io_num = cand[i], .scl_io_num = cand[j],
                .clk_source = I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt = 7,
                .flags.enable_internal_pullup = true,
            };
            if (i2c_new_master_bus(&cfg, &bus) != ESP_OK) continue;
            char line[96]; int o = 0;
            for (int a = 0x08; a < 0x78; a++)
                if (i2c_master_probe(bus, a, 5) == ESP_OK) o += snprintf(line + o, sizeof line - o, "0x%02x ", a);
            if (o) ESP_LOGW(TAG, "pin hunt: SDA=%d SCL=%d -> %s", cand[i], cand[j], line);
            i2c_del_master_bus(bus);
        }
    ESP_LOGW(TAG, "pin hunt: done");
}

// ---------------------------------------------------------------- gesture demo
static QueueHandle_t s_gq;

static void on_gesture(gesture_t g, void *ctx) { xQueueSend(s_gq, &g, 0); }

static void fill(uint16_t c)
{
    for (int i = 0; i < VIEW_W * BAND_LINES; i++) s_band[i] = c;
    for (int y0 = 0; y0 < VIEW_H; y0 += BAND_LINES) {
        esp_lcd_panel_draw_bitmap(s_panel, 0, y0, VIEW_W, y0 + BAND_LINES, s_band);
        xSemaphoreTake(s_flush_done, portMAX_DELAY);
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "=== Holoneo: gesture demo (wave=green, hover=blue, up=magenta) ===");
    display_init();
    draw_test_pattern();
    i2c_scan();
    {
        i2c_master_dev_handle_t d;
        i2c_device_config_t dc = { .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = 0x41, .scl_speed_hz = 100000 };
        if (i2c_master_bus_add_device(s_i2c, &dc, &d) == ESP_OK) {
            for (int k = 0; k < 2; k++) {
                uint8_t r = 0, v[0x40];
                if (i2c_master_transmit_receive(d, &r, 1, v, sizeof v, 50) == ESP_OK) {
                    char line[3 * 0x40 + 1]; int o = 0;
                    for (int i = 0; i < 0x40; i++) o += snprintf(line + o, sizeof line - o, "%02x ", v[i]);
                    ESP_LOGI(TAG, "epl regs: %s", line);
                }
                vTaskDelay(pdMS_TO_TICKS(100));
            }
            i2c_master_bus_rm_device(d);
        }
    }
    s_gq = xQueueCreate(4, sizeof(gesture_t));
    if (gesture_start(s_i2c, on_gesture, NULL) != ESP_OK) return;
    {   // log the register state after init, for cold-start diagnosis
        i2c_master_dev_handle_t d;
        i2c_device_config_t dc = { .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = 0x41, .scl_speed_hz = 100000 };
        if (i2c_master_bus_add_device(s_i2c, &dc, &d) == ESP_OK) {
            uint8_t r = 0, v[0x28];
            if (i2c_master_transmit_receive(d, &r, 1, v, sizeof v, 50) == ESP_OK) {
                char line[3 * 0x28 + 1]; int o = 0;
                for (int i = 0; i < 0x28; i++) o += snprintf(line + o, sizeof line - o, "%02x ", v[i]);
                ESP_LOGI(TAG, "epl regs after init: %s", line);
            }
            i2c_master_bus_rm_device(d);
        }
    }
    for (;;) {
        gesture_t g;
        xQueueReceive(s_gq, &g, portMAX_DELAY);
        fill(g == GESTURE_WAVE ? rgb565(0, 200, 0) : g == GESTURE_HOVER ? rgb565(0, 0, 255) : rgb565(255, 0, 255));
        vTaskDelay(pdMS_TO_TICKS(400));
        draw_test_pattern();
    }
}
