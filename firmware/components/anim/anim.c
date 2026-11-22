#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "display.h"
#include "anim.h"

static const char *TAG = "anim";

#define MAX_FRAME   (384 * 1024)
#define MAX_MJPEG   (6 * 1024 * 1024)

// ------------------------------------------------------------------ byte source (file or memory)
typedef struct {
    FILE *f;
    const uint8_t *mem;
    size_t len, pos;
} src_t;

static size_t src_read(src_t *s, void *dst, size_t n)
{
    if (s->f) return fread(dst, 1, n, s->f);
    if (s->pos >= s->len) return 0;
    if (n > s->len - s->pos) n = s->len - s->pos;
    memcpy(dst, s->mem + s->pos, n);
    s->pos += n;
    return n;
}
static void src_seek(src_t *s, size_t off) { if (s->f) fseek(s->f, off, SEEK_SET); else s->pos = off; }
static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

// ------------------------------------------------------------------ clip state
typedef enum { CLIP_AVI, CLIP_MJPEG } clip_kind_t;

typedef struct {
    clip_kind_t kind;
    src_t src;
    uint8_t *owned;         // memory to free when done
    size_t movi_start, movi_end, cursor;
    int us_per_frame;
    bool loop;
    int width, height, frames;
} clip_t;

// Finds the AVI frame rate and the 'movi' list. Returns false if not an AVI.
static bool avi_open(clip_t *c)
{
    uint8_t h[12];
    src_seek(&c->src, 0);
    if (src_read(&c->src, h, 12) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "AVI ", 4)) return false;
    size_t end = 8 + le32(h + 4), pos = 12;
    c->us_per_frame = 66666;
    while (pos + 8 <= end) {
        uint8_t ch[12];
        src_seek(&c->src, pos);
        if (src_read(&c->src, ch, 8) != 8) break;
        uint32_t len = le32(ch + 4);
        if (!memcmp(ch, "LIST", 4)) {
            src_read(&c->src, ch + 8, 4);
            if (!memcmp(ch + 8, "movi", 4)) { c->movi_start = pos + 12; c->movi_end = pos + 8 + len; return true; }
            if (!memcmp(ch + 8, "hdrl", 4)) { pos += 12; continue; }   // descend into the header list
        } else if (!memcmp(ch, "avih", 4)) {
            uint8_t a[40];
            src_read(&c->src, a, 40);
            if (le32(a)) c->us_per_frame = le32(a);
            c->frames = le32(a + 16);
            c->width = le32(a + 32);
            c->height = le32(a + 36);
        }
        pos += 8 + len + (len & 1);
    }
    return false;
}

// Reads the next JPEG frame into buf. Returns its length, 0 at the end of the clip.
static size_t next_frame(clip_t *c, uint8_t *buf)
{
    if (c->kind == CLIP_MJPEG) {
        const uint8_t *m = c->src.mem;
        size_t i = c->cursor;
        while (i + 1 < c->src.len && !(m[i] == 0xFF && m[i + 1] == 0xD8)) i++;
        size_t j = i + 2;
        while (j + 1 < c->src.len && !(m[j] == 0xFF && m[j + 1] == 0xD9)) j++;
        if (j + 1 >= c->src.len) return 0;
        size_t n = j + 2 - i;
        if (n > MAX_FRAME) { c->cursor = j + 2; return next_frame(c, buf); }
        memcpy(buf, m + i, n);
        c->cursor = j + 2;
        return n;
    }
    while (c->cursor + 8 <= c->movi_end) {
        uint8_t ch[8];
        src_seek(&c->src, c->cursor);
        if (src_read(&c->src, ch, 8) != 8) return 0;
        uint32_t len = le32(ch + 4);
        if (!memcmp(ch, "LIST", 4)) { c->cursor += 12; continue; }   // 'rec ' lists: descend
        c->cursor += 8 + len + (len & 1);
        if (ch[2] == 'd' && (ch[3] == 'c' || ch[3] == 'b') && len > 0 && len <= MAX_FRAME) {
            if (src_read(&c->src, buf, len) != len) return 0;
            return len;
        }
    }
    return 0;
}

static void rewind_clip(clip_t *c) { c->cursor = c->kind == CLIP_AVI ? c->movi_start : 0; }

static void close_clip(clip_t *c)
{
    if (c->src.f) fclose(c->src.f);
    free(c->owned);
    memset(c, 0, sizeof *c);
}

// ------------------------------------------------------------------ player task
typedef struct { char path[96]; uint8_t *buf; size_t len; bool loop; int fps; } cmd_t;

static QueueHandle_t s_cmds, s_present_q;
static SemaphoreHandle_t s_buf_free[2], s_idle;
static uint16_t *s_fb[2];
static uint8_t *s_jpg;
static volatile bool s_stop, s_playing;
static float s_fps;
typedef struct { uint8_t *buf; size_t len; } frame_t;
static QueueHandle_t s_stream_q;
static void stream_task(void *arg);
static volatile int64_t s_present_us;   // accumulated time spent sending frames to the panel

static void present_task(void *arg)
{
    int idx;
    for (;;) {
        xQueueReceive(s_present_q, &idx, portMAX_DELAY);
        int64_t t0 = esp_timer_get_time();
        display_present(s_fb[idx]);
        s_present_us += esp_timer_get_time() - t0;
        xSemaphoreGive(s_buf_free[idx]);
    }
}

static bool open_cmd(const cmd_t *cmd, clip_t *c)
{
    memset(c, 0, sizeof *c);
    c->loop = cmd->loop;
    if (cmd->buf) {
        c->src.mem = c->owned = cmd->buf;
        c->src.len = cmd->len;
    } else {
        const char *ext = strrchr(cmd->path, '.');
        bool mjpeg = ext && (!strcasecmp(ext, ".mjpeg") || !strcasecmp(ext, ".mjpg"));
        FILE *f = fopen(cmd->path, "rb");
        if (!f) { ESP_LOGE(TAG, "can't open %s", cmd->path); return false; }
        if (mjpeg) {   // concatenated JPEGs: load into PSRAM
            fseek(f, 0, SEEK_END);
            size_t n = ftell(f);
            fseek(f, 0, SEEK_SET);
            uint8_t *b = n <= MAX_MJPEG ? heap_caps_malloc(n, MALLOC_CAP_SPIRAM) : NULL;
            if (!b || fread(b, 1, n, f) != n) { fclose(f); free(b); ESP_LOGE(TAG, "%s too big", cmd->path); return false; }
            fclose(f);
            c->src.mem = c->owned = b;
            c->src.len = n;
        } else {
            setvbuf(f, NULL, _IOFBF, 16 * 1024);
            c->src.f = f;
        }
    }
    if (avi_open(c)) {
        c->kind = CLIP_AVI;
        ESP_LOGI(TAG, "AVI %dx%d, %d frames, %.1f fps", c->width, c->height, c->frames, 1e6 / c->us_per_frame);
    } else if (!c->src.f) {
        c->kind = CLIP_MJPEG;
        c->us_per_frame = 1000000 / 15;
    } else {
        ESP_LOGE(TAG, "not an AVI file");
        close_clip(c);
        return false;
    }
    if (cmd->fps > 0) c->us_per_frame = 1000000 / cmd->fps;
    rewind_clip(c);
    return true;
}

static void player_task(void *arg)
{
    cmd_t cmd;
    for (;;) {
        xQueueReceive(s_cmds, &cmd, portMAX_DELAY);
        clip_t c;
        s_stop = false;
        if (!open_cmd(&cmd, &c)) { free(cmd.buf); continue; }
        s_playing = true;
        xSemaphoreTake(s_idle, 0);
        int cur = 0, shown = 0;
        int64_t t_next = esp_timer_get_time(), t_stat = t_next, read_us = 0, dec_us = 0, wait_us = 0;
        size_t bytes = 0;
        s_present_us = 0;
        while (!s_stop) {
            int64_t t0 = esp_timer_get_time();
            size_t n = next_frame(&c, s_jpg);
            if (n == 0) {
                if (!c.loop) break;
                rewind_clip(&c);
                if ((n = next_frame(&c, s_jpg)) == 0) break;
            }
            int64_t t1 = esp_timer_get_time();
            xSemaphoreTake(s_buf_free[cur], portMAX_DELAY);      // wait until this buffer was sent
            int64_t t2 = esp_timer_get_time();
            if (display_jpeg_decode(s_fb[cur], s_jpg, n) != ESP_OK) { xSemaphoreGive(s_buf_free[cur]); continue; }
            int64_t t3 = esp_timer_get_time();
            read_us += t1 - t0; wait_us += t2 - t1; dec_us += t3 - t2; bytes += n;
            int64_t now = esp_timer_get_time();
            if (t_next > now) vTaskDelay(pdMS_TO_TICKS((t_next - now) / 1000));
            t_next = (t_next > now - 200000 ? t_next : now) + c.us_per_frame;   // don't try to catch up after stalls
            xQueueSend(s_present_q, &cur, portMAX_DELAY);
            cur ^= 1;
            if (++shown % 30 == 0) {
                int64_t t = esp_timer_get_time();
                s_fps = 30e6f / (t - t_stat);
                t_stat = t;
                if (shown % 150 == 0)
                    ESP_LOGI(TAG, "%.1f fps | per frame: read %.1f ms, wait %.1f ms, decode %.1f ms, send %.1f ms, %u bytes",
                             s_fps, read_us / 150e3, wait_us / 150e3, dec_us / 150e3, s_present_us / 150e3, (unsigned)(bytes / 150));
                if (shown % 150 == 0) { read_us = wait_us = dec_us = 0; bytes = 0; s_present_us = 0; }
            }
        }
        for (int i = 0; i < 2; i++) { xSemaphoreTake(s_buf_free[i], portMAX_DELAY); xSemaphoreGive(s_buf_free[i]); }
        close_clip(&c);
        s_playing = false;
        s_fps = 0;
        xSemaphoreGive(s_idle);
        ESP_LOGI(TAG, "stopped after %d frames", shown);
    }
}

esp_err_t anim_init(void)
{
    s_cmds = xQueueCreate(1, sizeof(cmd_t));
    s_present_q = xQueueCreate(2, sizeof(int));
    s_idle = xSemaphoreCreateBinary();
    xSemaphoreGive(s_idle);
    for (int i = 0; i < 2; i++) {
        s_buf_free[i] = xSemaphoreCreateBinary();
        xSemaphoreGive(s_buf_free[i]);
        s_fb[i] = heap_caps_aligned_calloc(16, DISPLAY_W * DISPLAY_H, 2, MALLOC_CAP_SPIRAM);   // SIMD decoder needs 16-byte alignment
    }
    s_jpg = heap_caps_malloc(MAX_FRAME, MALLOC_CAP_SPIRAM);
    if (!s_fb[0] || !s_fb[1] || !s_jpg) return ESP_ERR_NO_MEM;
    xTaskCreatePinnedToCore(present_task, "present", 3072, NULL, 6, NULL, 0);
    xTaskCreatePinnedToCore(player_task, "anim", 6144, NULL, 4, NULL, 1);
    s_stream_q = xQueueCreate(1, sizeof(frame_t));
    xTaskCreatePinnedToCore(stream_task, "stream", 4096, NULL, 4, NULL, 1);
    return ESP_OK;
}

void anim_stop(void)
{
    if (!s_playing) return;
    s_stop = true;
    xSemaphoreTake(s_idle, portMAX_DELAY);
    xSemaphoreGive(s_idle);
}

bool anim_playing(void) { return s_playing; }
float anim_fps(void) { return s_fps; }

static esp_err_t enqueue(cmd_t *cmd)
{
    anim_stop();
    return xQueueSend(s_cmds, cmd, pdMS_TO_TICKS(1000)) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t anim_play_file(const char *path, bool loop, int fps)
{
    cmd_t cmd = { .loop = loop, .fps = fps };
    strlcpy(cmd.path, path, sizeof cmd.path);
    return enqueue(&cmd);
}

esp_err_t anim_play_mem(uint8_t *buf, size_t len, bool loop, int fps)
{
    cmd_t cmd = { .buf = buf, .len = len, .loop = loop, .fps = fps };
    esp_err_t e = enqueue(&cmd);
    if (e != ESP_OK) free(buf);
    return e;
}

// Live frames: the caller hands over a PSRAM buffer; a separate task decodes and shows it so the
// network task can already receive the next frame. If frames arrive faster than they can be shown,
// the newest waiting frame replaces the older one (low latency instead of a growing backlog).
static void stream_task(void *arg)
{
    int cur = 0;
    frame_t f;
    for (;;) {
        xQueueReceive(s_stream_q, &f, portMAX_DELAY);
        xSemaphoreTake(s_buf_free[cur], portMAX_DELAY);
        if (display_jpeg_decode(s_fb[cur], f.buf, f.len) == ESP_OK) {
            xQueueSend(s_present_q, &cur, portMAX_DELAY);
            cur ^= 1;
        } else {
            xSemaphoreGive(s_buf_free[cur]);
        }
        free(f.buf);
    }
}

esp_err_t anim_stream_frame(uint8_t *jpg, size_t len)
{
    if (s_playing) anim_stop();
    frame_t f = { jpg, len }, old;
    if (xQueueSend(s_stream_q, &f, 0) != pdTRUE) {
        if (xQueueReceive(s_stream_q, &old, 0) == pdTRUE) free(old.buf);   // drop the stale frame
        if (xQueueSend(s_stream_q, &f, 0) != pdTRUE) { free(jpg); return ESP_ERR_TIMEOUT; }
    }
    return ESP_OK;
}

esp_err_t anim_show_frame(const uint8_t *jpg, size_t len)
{
    uint8_t *copy = heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
    if (!copy) return ESP_ERR_NO_MEM;
    memcpy(copy, jpg, len);
    return anim_stream_frame(copy, len);
}
