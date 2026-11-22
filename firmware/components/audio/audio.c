#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "driver/i2s_std.h"
#include "driver/i2s_tdm.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "board.h"
#include "audio.h"

static const char *TAG = "audio";
static esp_codec_dev_handle_t s_spk, s_mic;
static SemaphoreHandle_t s_spk_lock, s_mic_lock;
static int s_volume = 60;

esp_err_t audio_init(void)
{
    s_spk_lock = xSemaphoreCreateMutex();
    s_mic_lock = xSemaphoreCreateMutex();
    i2c_master_bus_handle_t bus = board_i2c();
    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();

    // speaker: I2S0 standard mode -> ES8311
    i2s_chan_handle_t tx;
    i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    cc.auto_clear = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&cc, &tx, NULL), TAG, "i2s0");
    i2s_std_config_t std = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(16000),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = { .mclk = BOARD_SPK_MCLK, .bclk = BOARD_SPK_BCLK, .ws = BOARD_SPK_WS, .dout = BOARD_SPK_DOUT, .din = I2S_GPIO_UNUSED },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(tx, &std), TAG, "i2s0 std");
    audio_codec_i2s_cfg_t i2s0 = { .port = I2S_NUM_0, .tx_handle = tx };
    audio_codec_i2c_cfg_t i2c_spk = { .port = I2C_NUM_0, .addr = ES8311_CODEC_DEFAULT_ADDR, .bus_handle = bus };
    es8311_codec_cfg_t es8311 = {
        .ctrl_if = audio_codec_new_i2c_ctrl(&i2c_spk), .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC, .pa_pin = BOARD_PA_EN, .use_mclk = true,
        .hw_gain = { .pa_voltage = 5.0, .codec_dac_voltage = 3.3 },
    };
    esp_codec_dev_cfg_t spk_cfg = { .dev_type = ESP_CODEC_DEV_TYPE_OUT, .codec_if = es8311_codec_new(&es8311),
                                    .data_if = audio_codec_new_i2s_data(&i2s0) };
    s_spk = esp_codec_dev_new(&spk_cfg);
    ESP_RETURN_ON_FALSE(s_spk, ESP_FAIL, TAG, "ES8311");

    // microphones: I2S1 TDM (4 slots) <- ES7210, mics on slots 0 and 2
    i2s_chan_handle_t rx;
    i2s_chan_config_t cc1 = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    ESP_RETURN_ON_ERROR(i2s_new_channel(&cc1, NULL, &rx), TAG, "i2s1");
    i2s_tdm_config_t tdm = {
        .clk_cfg = I2S_TDM_CLK_DEFAULT_CONFIG(AUDIO_MIC_RATE),
        .slot_cfg = I2S_TDM_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO,
                        I2S_TDM_SLOT0 | I2S_TDM_SLOT1 | I2S_TDM_SLOT2 | I2S_TDM_SLOT3),
        .gpio_cfg = { .mclk = BOARD_MIC_MCLK, .bclk = BOARD_MIC_BCLK, .ws = BOARD_MIC_WS, .dout = I2S_GPIO_UNUSED, .din = BOARD_MIC_DIN },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_tdm_mode(rx, &tdm), TAG, "i2s1 tdm");
    audio_codec_i2s_cfg_t i2s1 = { .port = I2S_NUM_1, .rx_handle = rx };
    audio_codec_i2c_cfg_t i2c_mic = { .port = I2C_NUM_0, .addr = ES7210_CODEC_DEFAULT_ADDR, .bus_handle = bus };
    es7210_codec_cfg_t es7210 = { .ctrl_if = audio_codec_new_i2c_ctrl(&i2c_mic),
                                  .mic_selected = ES7210_SEL_MIC1 | ES7210_SEL_MIC2 | ES7210_SEL_MIC3 | ES7210_SEL_MIC4 };
    esp_codec_dev_cfg_t mic_cfg = { .dev_type = ESP_CODEC_DEV_TYPE_IN, .codec_if = es7210_codec_new(&es7210),
                                    .data_if = audio_codec_new_i2s_data(&i2s1) };
    s_mic = esp_codec_dev_new(&mic_cfg);
    ESP_RETURN_ON_FALSE(s_mic, ESP_FAIL, TAG, "ES7210");
    ESP_LOGI(TAG, "ready");
    return ESP_OK;
}

void audio_set_volume(int vol)
{
    s_volume = vol < 0 ? 0 : vol > 100 ? 100 : vol;
}
int audio_get_volume(void) { return s_volume; }

esp_err_t audio_play(const int16_t *pcm, size_t frames, int channels, int rate)
{
    ESP_RETURN_ON_FALSE(channels == 1 || channels == 2, ESP_ERR_INVALID_ARG, TAG, "channels");
    xSemaphoreTake(s_spk_lock, portMAX_DELAY);
    esp_codec_dev_sample_info_t fs = { .bits_per_sample = 16, .channel = 2, .sample_rate = rate };
    esp_err_t err = ESP_OK;
    if (esp_codec_dev_open(s_spk, &fs) != ESP_CODEC_DEV_OK) { err = ESP_FAIL; goto out; }
    esp_codec_dev_set_out_vol(s_spk, s_volume);
    // convert to stereo in chunks
    const size_t chunk = 1024;
    int16_t *buf = heap_caps_malloc(chunk * 4, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (!buf) { err = ESP_ERR_NO_MEM; goto close; }
    for (size_t i = 0; i < frames; i += chunk) {
        size_t n = frames - i < chunk ? frames - i : chunk;
        for (size_t k = 0; k < n; k++) {
            if (channels == 2) { buf[2 * k] = pcm[2 * (i + k)]; buf[2 * k + 1] = pcm[2 * (i + k) + 1]; }
            else buf[2 * k] = buf[2 * k + 1] = pcm[i + k];
        }
        esp_codec_dev_write(s_spk, buf, n * 4);
    }
    free(buf);
close:
    esp_codec_dev_close(s_spk);
out:
    xSemaphoreGive(s_spk_lock);
    return err;
}

esp_err_t audio_record(int16_t *out, size_t frames)
{
    xSemaphoreTake(s_mic_lock, portMAX_DELAY);
    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = 16, .channel = 4, .sample_rate = AUDIO_MIC_RATE,
        .channel_mask = ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0) | ESP_CODEC_DEV_MAKE_CHANNEL_MASK(1) |
                        ESP_CODEC_DEV_MAKE_CHANNEL_MASK(2) | ESP_CODEC_DEV_MAKE_CHANNEL_MASK(3),
    };
    esp_err_t err = ESP_OK;
    if (esp_codec_dev_open(s_mic, &fs) != ESP_CODEC_DEV_OK) { err = ESP_FAIL; goto out; }
    esp_codec_dev_set_in_gain(s_mic, 30.0);
    const size_t chunk = 512;
    int16_t *buf = heap_caps_malloc(chunk * 4 * 2, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (!buf) { err = ESP_ERR_NO_MEM; goto close; }
    for (size_t i = 0; i < frames; i += chunk) {
        size_t n = frames - i < chunk ? frames - i : chunk;
        if (esp_codec_dev_read(s_mic, buf, n * 4 * 2) != ESP_CODEC_DEV_OK) { err = ESP_FAIL; break; }
        for (size_t k = 0; k < n; k++) out[i + k] = (buf[4 * k] + buf[4 * k + 2]) / 2;   // mics on slots 0 and 2
    }
    free(buf);
close:
    esp_codec_dev_close(s_mic);
out:
    xSemaphoreGive(s_mic_lock);
    return err;
}
