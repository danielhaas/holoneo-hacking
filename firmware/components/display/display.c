#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "jpeg_decoder.h"
#include "esp_jpeg_dec.h"
#include "board.h"
#include "display.h"
#include "font_mono20.h"

static const char *TAG = "display";

#define BAND_LINES 20   // 2 x 12.8 KB of internal DMA RAM

static esp_lcd_panel_handle_t s_panel;
static SemaphoreHandle_t s_band_free, s_mutex, s_present_lock;
static uint16_t *s_fb;       // PSRAM, DISPLAY_W*DISPLAY_H, big-endian RGB565 (panel byte order)
static uint16_t *s_band[2];  // internal DMA RAM: copy one band while the other is being sent

static inline uint16_t be565(uint32_t rgb)
{
    uint8_t r = rgb >> 16, g = rgb >> 8, b = rgb;
    uint16_t c = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
    return (c >> 8) | (c << 8);
}

static bool on_flush_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *e, void *ctx)
{
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_band_free, &woken);
    return woken == pdTRUE;
}

void display_lock(void) { xSemaphoreTakeRecursive(s_mutex, portMAX_DELAY); }
void display_unlock(void) { xSemaphoreGiveRecursive(s_mutex); }

void display_backlight(bool on) { gpio_set_level(BOARD_LCD_BL, on); }

esp_err_t display_init(void)
{
    s_mutex = xSemaphoreCreateRecursiveMutex();
    s_present_lock = xSemaphoreCreateMutex();
    s_band_free = xSemaphoreCreateCounting(2, 2);
    s_fb = heap_caps_aligned_calloc(16, DISPLAY_W * DISPLAY_H, 2, MALLOC_CAP_SPIRAM);
    s_band[0] = heap_caps_malloc(DISPLAY_W * BAND_LINES * 2, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    s_band[1] = heap_caps_malloc(DISPLAY_W * BAND_LINES * 2, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    ESP_RETURN_ON_FALSE(s_fb && s_band[0] && s_band[1], ESP_ERR_NO_MEM, TAG, "framebuffer alloc");

    gpio_config_t out = { .pin_bit_mask = (1ULL << BOARD_LCD_AUX) | (1ULL << BOARD_LCD_BL), .mode = GPIO_MODE_OUTPUT };
    gpio_config(&out);
    gpio_set_level(BOARD_LCD_AUX, 1);
    gpio_set_level(BOARD_LCD_BL, 0);

    spi_bus_config_t bus = {
        .mosi_io_num = BOARD_LCD_MOSI, .miso_io_num = -1, .sclk_io_num = BOARD_LCD_SCLK,
        .quadwp_io_num = -1, .quadhd_io_num = -1, .max_transfer_sz = DISPLAY_W * BAND_LINES * 2,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(BOARD_LCD_HOST, &bus, SPI_DMA_CH_AUTO), TAG, "spi bus");
    esp_lcd_panel_io_handle_t io;
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = BOARD_LCD_CS, .dc_gpio_num = BOARD_LCD_DC, .spi_mode = 0,
        .pclk_hz = DISPLAY_SPI_HZ, .trans_queue_depth = 4,
        .lcd_cmd_bits = 8, .lcd_param_bits = 8, .on_color_trans_done = on_flush_done,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)BOARD_LCD_HOST, &io_cfg, &io), TAG, "io");
    esp_lcd_panel_dev_config_t pcfg = { .reset_gpio_num = BOARD_LCD_RST, .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB, .bits_per_pixel = 16 };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7789(io, &pcfg, &s_panel), TAG, "panel");
    esp_lcd_panel_reset(s_panel);
    esp_lcd_panel_init(s_panel);
    esp_lcd_panel_invert_color(s_panel, true);
    // Landscape as seen in the reflection (see docs/hardware.md).
    esp_lcd_panel_swap_xy(s_panel, true);
    esp_lcd_panel_mirror(s_panel, true, false);
    display_flush();                 // push the black framebuffer before switching on
    esp_lcd_panel_disp_on_off(s_panel, true);
    gpio_set_level(BOARD_LCD_BL, 1);
    ESP_LOGI(TAG, "ready (%dx%d)", DISPLAY_W, DISPLAY_H);
    return ESP_OK;
}

void display_present(const uint16_t *buf)
{
    xSemaphoreTake(s_present_lock, portMAX_DELAY);
    for (int i = 0, y0 = 0; y0 < DISPLAY_H; y0 += BAND_LINES, i ^= 1) {
        xSemaphoreTake(s_band_free, portMAX_DELAY);          // band buffer i is no longer being sent
        memcpy(s_band[i], buf + y0 * DISPLAY_W, DISPLAY_W * BAND_LINES * 2);
        esp_lcd_panel_draw_bitmap(s_panel, 0, y0, DISPLAY_W, y0 + BAND_LINES, s_band[i]);
    }
    xSemaphoreTake(s_band_free, portMAX_DELAY);              // wait until both bands are sent
    xSemaphoreTake(s_band_free, portMAX_DELAY);
    xSemaphoreGive(s_band_free);
    xSemaphoreGive(s_band_free);
    xSemaphoreGive(s_present_lock);
}

void display_flush(void)
{
    display_lock();
    display_present(s_fb);
    display_unlock();
}

void display_rect(int x, int y, int w, int h, uint32_t rgb)
{
    uint16_t c = be565(rgb);
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > DISPLAY_W) w = DISPLAY_W - x;
    if (y + h > DISPLAY_H) h = DISPLAY_H - y;
    display_lock();
    for (int yy = y; yy < y + h; yy++)
        for (int xx = x; xx < x + w; xx++) s_fb[yy * DISPLAY_W + xx] = c;
    display_unlock();
}

void display_fill(uint32_t rgb) { display_rect(0, 0, DISPLAY_W, DISPLAY_H, rgb); }

// Blend colour fg over the framebuffer pixel with alpha a (0..15).
static inline void blend(uint16_t *px, uint32_t fg, int a)
{
    if (a == 0) return;
    if (a == 15) { *px = be565(fg); return; }
    uint16_t c = (*px >> 8) | (*px << 8);
    int r = (c >> 11) << 3, g = ((c >> 5) & 0x3F) << 2, b = (c & 0x1F) << 3;
    int fr = fg >> 16 & 0xFF, fgc = fg >> 8 & 0xFF, fb = fg & 0xFF;
    r += (fr - r) * a / 15; g += (fgc - g) * a / 15; b += (fb - b) * a / 15;
    *px = be565((uint32_t)r << 16 | g << 8 | b);
}

static void draw_char(int x, int y, char ch, uint32_t rgb, int scale)
{
    if (ch < 32 || ch > 126) ch = '?';
    const uint8_t *g = font_mono20 + (ch - 32) * (FONT_MONO20_W / 2) * FONT_MONO20_H;
    for (int gy = 0; gy < FONT_MONO20_H; gy++)
        for (int gx = 0; gx < FONT_MONO20_W; gx++) {
            uint8_t byte = g[gy * (FONT_MONO20_W / 2) + gx / 2];
            int a = (gx & 1) ? (byte & 0x0F) : (byte >> 4);
            if (!a) continue;
            for (int sy = 0; sy < scale; sy++)
                for (int sx = 0; sx < scale; sx++) {
                    int px = x + gx * scale + sx, py = y + gy * scale + sy;
                    if (px >= 0 && py >= 0 && px < DISPLAY_W && py < DISPLAY_H) blend(&s_fb[py * DISPLAY_W + px], rgb, a);
                }
        }
}

int display_text_cols(int scale) { return DISPLAY_W / (FONT_MONO20_W * scale); }
int display_text_rows(int scale) { return DISPLAY_H / (FONT_MONO20_H * scale); }

void display_text(int x, int y, const char *s, uint32_t rgb, int scale)
{
    if (scale < 1) scale = 1;
    display_lock();
    int cx = x;
    for (; *s; s++) {
        if (*s == '\n') { cx = x; y += FONT_MONO20_H * scale; continue; }
        draw_char(cx, y, *s, rgb, scale);
        cx += FONT_MONO20_W * scale;
    }
    display_unlock();
}

void display_text_centered(const char *s, uint32_t rgb, int scale)
{
    if (scale < 1) scale = 1;
    int cols = display_text_cols(scale), max_rows = display_text_rows(scale);
    // word-wrap into lines
    char lines[16][64];
    int n = 0;
    const char *p = s;
    while (*p && n < max_rows && n < 16) {
        int len = 0, last_space = -1;
        while (p[len] && p[len] != '\n' && len < cols && len < 63) { if (p[len] == ' ') last_space = len; len++; }
        if (p[len] && p[len] != '\n' && len == cols && last_space > 0) len = last_space;
        memcpy(lines[n], p, len); lines[n][len] = 0; n++;
        p += len;
        while (*p == ' ') p++;
        if (*p == '\n') p++;
    }
    int lh = FONT_MONO20_H * scale, y = (DISPLAY_H - n * lh) / 2;
    display_lock();
    for (int i = 0; i < n; i++) {
        int w = strlen(lines[i]) * FONT_MONO20_W * scale;
        display_text((DISPLAY_W - w) / 2, y + i * lh, lines[i], rgb, scale);
    }
    display_unlock();
}

esp_err_t display_jpeg_decode(uint16_t *dst, const uint8_t *jpg, size_t len)
{
    esp_jpeg_image_cfg_t cfg = { .indata = (uint8_t *)jpg, .indata_size = len, .out_format = JPEG_IMAGE_FORMAT_RGB565,
                                 .flags.swap_color_bytes = 1 };
    esp_jpeg_image_output_t info;
    if (esp_jpeg_get_image_info(&cfg, &info) != ESP_OK) return ESP_ERR_INVALID_ARG;
    // Fast path: exactly screen-sized -> SIMD decoder (esp_new_jpeg) straight into the
    // destination, which must be 16-byte aligned. ~4x faster than the generic decoder.
    if (info.width == DISPLAY_W && info.height == DISPLAY_H && ((uintptr_t)dst & 15) == 0) {
        jpeg_dec_config_t dc = DEFAULT_JPEG_DEC_CONFIG();
        dc.output_type = JPEG_PIXEL_FORMAT_RGB565_BE;
        jpeg_dec_handle_t h;
        if (jpeg_dec_open(&dc, &h) != JPEG_ERR_OK) return ESP_FAIL;
        jpeg_dec_io_t io = { .inbuf = (uint8_t *)jpg, .inbuf_len = len, .outbuf = (uint8_t *)dst };
        jpeg_dec_header_info_t hi;
        jpeg_error_t je = jpeg_dec_parse_header(h, &io, &hi);
        if (je == JPEG_ERR_OK) je = jpeg_dec_process(h, &io);
        jpeg_dec_close(h);
        return je == JPEG_ERR_OK ? ESP_OK : ESP_FAIL;
    }
    static const esp_jpeg_image_scale_t scales[] = { JPEG_IMAGE_SCALE_0, JPEG_IMAGE_SCALE_1_2, JPEG_IMAGE_SCALE_1_4, JPEG_IMAGE_SCALE_1_8 };
    int si = 0;
    while (si < 3 && ((info.width >> si) > DISPLAY_W || (info.height >> si) > DISPLAY_H)) si++;
    cfg.out_scale = scales[si];
    // decoder output is rounded up to whole MCUs (16 px)
    size_t out_size = ((info.width + 15) / 16 * 16 >> si) * ((info.height + 15) / 16 * 16 >> si) * 2 + 64;
    uint8_t *out = heap_caps_malloc(out_size, MALLOC_CAP_SPIRAM);
    if (!out) return ESP_ERR_NO_MEM;
    cfg.outbuf = out;
    cfg.outbuf_size = out_size;
    esp_jpeg_image_output_t res;
    esp_err_t err = esp_jpeg_decode(&cfg, &res);
    if (err == ESP_OK) {
        int ow = res.width, oh = res.height;
        int dw = ow < DISPLAY_W ? ow : DISPLAY_W, dh = oh < DISPLAY_H ? oh : DISPLAY_H;
        int dx = (DISPLAY_W - dw) / 2, dy = (DISPLAY_H - dh) / 2, sx = (ow - dw) / 2, sy = (oh - dh) / 2;
        memset(dst, 0, DISPLAY_W * DISPLAY_H * 2);
        for (int y = 0; y < dh; y++)
            memcpy(dst + (dy + y) * DISPLAY_W + dx, (uint16_t *)out + (sy + y) * ow + sx, dw * 2);
    }
    free(out);
    return err;
}

esp_err_t display_jpeg(const uint8_t *jpg, size_t len)
{
    display_lock();
    esp_err_t err = display_jpeg_decode(s_fb, jpg, len);
    display_unlock();
    return err;
}

static uint16_t *s_saved;

void display_snapshot(void)
{
    display_lock();
    if (!s_saved) s_saved = heap_caps_malloc(DISPLAY_W * DISPLAY_H * 2, MALLOC_CAP_SPIRAM);
    if (s_saved) memcpy(s_saved, s_fb, DISPLAY_W * DISPLAY_H * 2);
    display_unlock();
}

void display_restore(void)
{
    display_lock();
    if (s_saved) memcpy(s_fb, s_saved, DISPLAY_W * DISPLAY_H * 2);
    display_unlock();
    display_flush();
}
