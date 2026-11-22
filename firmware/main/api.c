// HTTP API on port 80. See docs/api.md.
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_heap_caps.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "cJSON.h"
#include "display.h"
#include "audio.h"
#include "net.h"
#include "anim.h"
#include "storage.h"
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include "api.h"

static const char *TAG = "api";
static httpd_handle_t s_server;
static api_status_cb_t s_status_cb;

#define MAX_IMAGE (2 * 1024 * 1024)
#define MAX_AUDIO (4 * 1024 * 1024)
#define MAX_TEXT  2048

// ------------------------------------------------------------------ helpers
static bool authorized(httpd_req_t *req)
{
    char hdr[64], q[96], tok[32];
    if (httpd_req_get_hdr_value_str(req, "Authorization", hdr, sizeof hdr) == ESP_OK &&
        strncmp(hdr, "Bearer ", 7) == 0 && strcmp(hdr + 7, net_token()) == 0) return true;
    if (httpd_req_get_url_query_str(req, q, sizeof q) == ESP_OK &&
        httpd_query_key_value(q, "token", tok, sizeof tok) == ESP_OK && strcmp(tok, net_token()) == 0) return true;
    return false;
}

#define REQUIRE_AUTH(req) do { if (!authorized(req)) { \
        httpd_resp_set_status(req, "401 Unauthorized"); \
        httpd_resp_set_hdr(req, "WWW-Authenticate", "Bearer"); \
        return httpd_resp_sendstr(req, "{\"error\":\"missing or wrong token\"}\n"); } } while (0)

static esp_err_t send_json(httpd_req_t *req, cJSON *j)
{
    char *s = cJSON_PrintUnformatted(j);
    httpd_resp_set_type(req, "application/json");
    esp_err_t e = httpd_resp_sendstr(req, s);
    cJSON_free(s);
    cJSON_Delete(j);
    return e;
}

static esp_err_t send_error(httpd_req_t *req, const char *status, const char *msg)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    char buf[160];
    snprintf(buf, sizeof buf, "{\"error\":\"%s\"}\n", msg);
    return httpd_resp_sendstr(req, buf);
}

static esp_err_t send_ok(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}\n");
}

// Reads the whole body into PSRAM (NUL-terminated). Caller frees.
static uint8_t *read_body(httpd_req_t *req, size_t max, size_t *len_out)
{
    size_t len = req->content_len;
    if (len == 0 || len > max) return NULL;
    uint8_t *buf = heap_caps_malloc(len + 1, MALLOC_CAP_SPIRAM);
    if (!buf) return NULL;
    size_t got = 0;
    while (got < len) {
        int r = httpd_req_recv(req, (char *)buf + got, len - got);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0) { free(buf); return NULL; }
        got += r;
    }
    buf[len] = 0;
    *len_out = len;
    return buf;
}

static int query_int(httpd_req_t *req, const char *key, int def)
{
    char q[128], v[16];
    if (httpd_req_get_url_query_str(req, q, sizeof q) == ESP_OK && httpd_query_key_value(q, key, v, sizeof v) == ESP_OK)
        return atoi(v);
    return def;
}

static uint32_t query_color(httpd_req_t *req, const char *key, uint32_t def)
{
    char q[128], v[16];
    if (httpd_req_get_url_query_str(req, q, sizeof q) == ESP_OK && httpd_query_key_value(q, key, v, sizeof v) == ESP_OK)
        return strtoul(v[0] == '#' ? v + 1 : v, NULL, 16);
    return def;
}

// ------------------------------------------------------------------ handlers
static esp_err_t h_status(httpd_req_t *req)
{
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "device", "holoneo");
    cJSON_AddStringToObject(j, "version", esp_app_get_description()->version);
    cJSON_AddStringToObject(j, "hostname", NET_HOSTNAME ".local");
    cJSON_AddStringToObject(j, "ip", net_ip());
    cJSON_AddNumberToObject(j, "rssi", net_rssi());
    char bssid[20];
    int ch;
    net_ap_info(bssid, sizeof bssid, &ch);
    cJSON_AddStringToObject(j, "bssid", bssid);
    cJSON_AddNumberToObject(j, "channel", ch);
    cJSON_AddNumberToObject(j, "uptime_s", esp_timer_get_time() / 1000000);
    cJSON_AddNumberToObject(j, "free_heap", heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(j, "free_psram", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    cJSON_AddNumberToObject(j, "volume", audio_get_volume());
    cJSON_AddBoolToObject(j, "playing", anim_playing());
    cJSON_AddNumberToObject(j, "fps", (int)(anim_fps() * 10) / 10.0);
    cJSON_AddBoolToObject(j, "sd_card", storage_sd_present());
    size_t used, total;
    storage_data_usage(&used, &total);
    cJSON_AddNumberToObject(j, "data_used_kb", used / 1024);
    cJSON_AddNumberToObject(j, "data_total_kb", total / 1024);
    if (s_status_cb) s_status_cb(j);
    return send_json(req, j);
}

static esp_err_t h_image(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    anim_stop();
    size_t len;
    uint8_t *jpg = read_body(req, MAX_IMAGE, &len);
    if (!jpg) return send_error(req, "413 Payload Too Large", "body missing or larger than 2 MB");
    esp_err_t e = display_jpeg(jpg, len);
    free(jpg);
    if (e != ESP_OK) return send_error(req, "415 Unsupported Media Type", "expected a baseline (non-progressive) JPEG");
    display_flush();
    return send_ok(req);
}

static esp_err_t h_text(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    anim_stop();
    size_t len;
    char *txt = (char *)read_body(req, MAX_TEXT, &len);
    if (!txt) return send_error(req, "400 Bad Request", "body must be 1..2048 bytes of text");
    int scale = query_int(req, "size", 1);
    uint32_t fg = query_color(req, "color", 0xFFFFFF), bg = query_color(req, "bg", 0x000000);
    display_lock();
    display_fill(bg);
    display_text_centered(txt, fg, scale < 1 ? 1 : scale > 4 ? 4 : scale);
    display_unlock();
    display_flush();
    free(txt);
    return send_ok(req);
}

static esp_err_t h_fill(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    anim_stop();
    display_fill(query_color(req, "color", 0x000000));
    display_flush();
    return send_ok(req);
}

static esp_err_t h_backlight(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    display_backlight(query_int(req, "on", 1) != 0);
    return send_ok(req);
}

static esp_err_t h_volume(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    audio_set_volume(query_int(req, "level", audio_get_volume()));
    return send_ok(req);
}

static const uint8_t *find_chunk(const uint8_t *w, size_t len, const char *id, uint32_t *clen)
{
    size_t p = 12;
    while (p + 8 <= len) {
        uint32_t l = w[p + 4] | w[p + 5] << 8 | w[p + 6] << 16 | (uint32_t)w[p + 7] << 24;
        if (memcmp(w + p, id, 4) == 0) { *clen = l; return w + p + 8; }
        p += 8 + l + (l & 1);
    }
    return NULL;
}

static esp_err_t h_audio(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    size_t len;
    uint8_t *wav = read_body(req, MAX_AUDIO, &len);
    if (!wav) return send_error(req, "413 Payload Too Large", "body missing or larger than 4 MB");
    uint32_t flen, dlen;
    const uint8_t *fmt, *data;
    if (len < 44 || memcmp(wav, "RIFF", 4) || memcmp(wav + 8, "WAVE", 4) ||
        !(fmt = find_chunk(wav, len, "fmt ", &flen)) || !(data = find_chunk(wav, len, "data", &dlen))) {
        free(wav);
        return send_error(req, "415 Unsupported Media Type", "expected a WAV file");
    }
    int format = fmt[0] | fmt[1] << 8, ch = fmt[2] | fmt[3] << 8, bits = fmt[14] | fmt[15] << 8;
    int rate = fmt[4] | fmt[5] << 8 | fmt[6] << 16 | fmt[7] << 24;
    if (format != 1 || bits != 16 || (ch != 1 && ch != 2)) {
        free(wav);
        return send_error(req, "415 Unsupported Media Type", "need 16-bit PCM, mono or stereo");
    }
    if (data + dlen > wav + len) dlen = wav + len - data;
    // reply first so the client isn't kept waiting while it plays
    send_ok(req);
    audio_play((const int16_t *)data, dlen / (2 * ch), ch, rate);
    free(wav);
    return ESP_OK;
}

static void put_le(uint8_t *p, uint32_t v, int n) { for (int i = 0; i < n; i++) p[i] = v >> (8 * i); }

static esp_err_t h_mic(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    int secs = query_int(req, "seconds", 3);
    if (secs < 1) secs = 1;
    if (secs > 30) secs = 30;
    size_t frames = secs * AUDIO_MIC_RATE;
    uint8_t *buf = heap_caps_malloc(44 + frames * 2, MALLOC_CAP_SPIRAM);
    if (!buf) return send_error(req, "500 Internal Server Error", "out of memory");
    if (audio_record((int16_t *)(buf + 44), frames) != ESP_OK) { free(buf); return send_error(req, "500 Internal Server Error", "record failed"); }
    memcpy(buf, "RIFF", 4); put_le(buf + 4, 36 + frames * 2, 4); memcpy(buf + 8, "WAVEfmt ", 8);
    put_le(buf + 16, 16, 4); put_le(buf + 20, 1, 2); put_le(buf + 22, 1, 2);
    put_le(buf + 24, AUDIO_MIC_RATE, 4); put_le(buf + 28, AUDIO_MIC_RATE * 2, 4); put_le(buf + 32, 2, 2); put_le(buf + 34, 16, 2);
    memcpy(buf + 36, "data", 4); put_le(buf + 40, frames * 2, 4);
    httpd_resp_set_type(req, "audio/wav");
    esp_err_t e = httpd_resp_send(req, (const char *)buf, 44 + frames * 2);
    free(buf);
    return e;
}

static esp_err_t h_wifi(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    size_t len;
    char *body = (char *)read_body(req, 512, &len);
    cJSON *j = body ? cJSON_Parse(body) : NULL;
    free(body);
    const cJSON *ssid = cJSON_GetObjectItem(j, "ssid"), *pw = cJSON_GetObjectItem(j, "password");
    if (!cJSON_IsString(ssid) || !ssid->valuestring[0]) { cJSON_Delete(j); return send_error(req, "400 Bad Request", "need {\"ssid\":..,\"password\":..}"); }
    send_ok(req);
    net_set_wifi(ssid->valuestring, cJSON_IsString(pw) ? pw->valuestring : "");
    cJSON_Delete(j);
    return ESP_OK;
}

static void restart_later(void *arg) { vTaskDelay(pdMS_TO_TICKS(500)); esp_restart(); }

static esp_err_t h_reboot(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    send_ok(req);
    xTaskCreate(restart_later, "restart", 2048, NULL, 5, NULL);
    return ESP_OK;
}

static esp_err_t h_ota(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part || req->content_len == 0 || req->content_len > part->size)
        return send_error(req, "400 Bad Request", "body must be a firmware image that fits the OTA slot");
    esp_ota_handle_t ota;
    if (esp_ota_begin(part, req->content_len, &ota) != ESP_OK) return send_error(req, "500 Internal Server Error", "ota begin failed");
    char *buf = malloc(4096);
    size_t got = 0;
    esp_err_t err = ESP_OK;
    while (got < req->content_len) {
        int r = httpd_req_recv(req, buf, 4096);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0) { err = ESP_FAIL; break; }
        if ((err = esp_ota_write(ota, buf, r)) != ESP_OK) break;
        got += r;
    }
    free(buf);
    if (err == ESP_OK) err = esp_ota_end(ota); else esp_ota_abort(ota);
    if (err == ESP_OK) err = esp_ota_set_boot_partition(part);
    if (err != ESP_OK) return send_error(req, "500 Internal Server Error", esp_err_to_name(err));
    ESP_LOGW(TAG, "OTA: wrote %u bytes to %s, rebooting", (unsigned)got, part->label);
    send_ok(req);
    xTaskCreate(restart_later, "restart", 2048, NULL, 5, NULL);
    return ESP_OK;
}

// ------------------------------------------------------------------ animation & files
#define MAX_CLIP (6 * 1024 * 1024)

// Accepts only /data/... and /sd/... without "..". Writes the filesystem path into out.
static bool safe_path(const char *p, char *out, size_t n)
{
    if (strncmp(p, "/data", 5) && strncmp(p, "/sd", 3)) return false;
    if (strstr(p, "..")) return false;
    strlcpy(out, p, n);
    size_t l = strlen(out);
    while (l > 1 && out[l - 1] == '/') out[--l] = 0;   // no trailing slash
    return true;
}

static esp_err_t h_play(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    char q[192], raw[128], path[128];
    bool loop = query_int(req, "loop", 1) != 0;
    int fps = query_int(req, "fps", 0);
    if (httpd_req_get_url_query_str(req, q, sizeof q) == ESP_OK && httpd_query_key_value(q, "path", raw, sizeof raw) == ESP_OK) {
        if (!safe_path(raw, path, sizeof path)) return send_error(req, "400 Bad Request", "path must be under /data or /sd");
        struct stat st;
        if (stat(path, &st) != 0) return send_error(req, "404 Not Found", "no such file");
        if (anim_play_file(path, loop, fps) != ESP_OK) return send_error(req, "500 Internal Server Error", "player busy");
        return send_ok(req);
    }
    size_t len;
    uint8_t *clip = read_body(req, MAX_CLIP, &len);
    if (!clip) return send_error(req, "400 Bad Request", "give ?path=... or upload an AVI/MJPEG clip (<= 6 MB)");
    if (anim_play_mem(clip, len, loop, fps) != ESP_OK) return send_error(req, "500 Internal Server Error", "player busy");
    return send_ok(req);
}

static esp_err_t h_stop(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    anim_stop();
    return send_ok(req);
}

static esp_err_t mkdirs(const char *path)
{
    char tmp[128];
    strlcpy(tmp, path, sizeof tmp);
    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/') continue;
        *p = 0;
        if (strcmp(tmp, "/data") && strcmp(tmp, "/sd")) mkdir(tmp, 0775);
        *p = '/';
    }
    return ESP_OK;
}

// GET /files/<dir> lists, GET /files/<file> downloads, PUT uploads, DELETE removes.
static esp_err_t h_files(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    char path[128], uri[160];
    strlcpy(uri, req->uri + strlen("/files"), sizeof uri);
    char *qm = strchr(uri, '?');
    if (qm) *qm = 0;
    if (!safe_path(uri, path, sizeof path)) return send_error(req, "400 Bad Request", "path must be under /files/data or /files/sd");
    struct stat st;
    bool exists = stat(path, &st) == 0;

    if (req->method == HTTP_GET) {
        if (!exists) return send_error(req, "404 Not Found", "no such file or directory");
        if (S_ISDIR(st.st_mode)) {
            DIR *d = opendir(path);
            if (!d) return send_error(req, "500 Internal Server Error", "can't list");
            cJSON *arr = cJSON_CreateArray();
            struct dirent *e;
            while ((e = readdir(d))) {
                char full[400];
                snprintf(full, sizeof full, "%s/%s", path, e->d_name);
                struct stat es;
                cJSON *o = cJSON_CreateObject();
                cJSON_AddStringToObject(o, "name", e->d_name);
                bool dir = e->d_type == DT_DIR;
                cJSON_AddBoolToObject(o, "dir", dir);
                if (!dir && stat(full, &es) == 0) cJSON_AddNumberToObject(o, "size", es.st_size);
                cJSON_AddItemToArray(arr, o);
            }
            closedir(d);
            return send_json(req, arr);
        }
        FILE *f = fopen(path, "rb");
        if (!f) return send_error(req, "500 Internal Server Error", "can't open");
        httpd_resp_set_type(req, "application/octet-stream");
        char *buf = malloc(4096);
        size_t n;
        while ((n = fread(buf, 1, 4096, f)) > 0)
            if (httpd_resp_send_chunk(req, buf, n) != ESP_OK) break;
        free(buf);
        fclose(f);
        return httpd_resp_send_chunk(req, NULL, 0);
    }
    if (req->method == HTTP_DELETE) {
        if (!exists) return send_error(req, "404 Not Found", "no such file");
        anim_stop();   // the player might have it open
        if ((S_ISDIR(st.st_mode) ? rmdir(path) : unlink(path)) != 0) return send_error(req, "500 Internal Server Error", strerror(errno));
        return send_ok(req);
    }
    // PUT: stream the body to the file
    if (req->content_len == 0) return send_error(req, "400 Bad Request", "empty body");
    anim_stop();
    mkdirs(path);
    char tmp[136];
    snprintf(tmp, sizeof tmp, "%s.part", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return send_error(req, "500 Internal Server Error", "can't create file (does the volume exist?)");
    char *buf = malloc(8192);
    size_t got = 0;
    bool ok = true;
    while (got < req->content_len) {
        int r = httpd_req_recv(req, buf, 8192);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0 || fwrite(buf, 1, r, f) != (size_t)r) { ok = false; break; }
        got += r;
    }
    free(buf);
    fclose(f);
    if (!ok) { unlink(tmp); return send_error(req, "500 Internal Server Error", "write failed (disk full?)"); }
    unlink(path);
    rename(tmp, path);
    ESP_LOGI(TAG, "stored %s (%u bytes)", path, (unsigned)got);
    return send_ok(req);
}

// WebSocket: clients connect to /ws?token=... and receive events as JSON text frames.
// Binary frames sent by the client are shown as JPEG frames (live streaming).
static esp_err_t h_ws(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        if (!authorized(req)) return ESP_FAIL;   // closes the connection during the handshake
        ESP_LOGI(TAG, "websocket client connected");
        return ESP_OK;
    }
    httpd_ws_frame_t f = {0};
    if (httpd_ws_recv_frame(req, &f, 0) != ESP_OK) return ESP_FAIL;
    if (!f.len) return ESP_OK;
    if (f.len > 512 * 1024) return ESP_FAIL;
    uint8_t *b = heap_caps_malloc(f.len, MALLOC_CAP_SPIRAM);
    if (!b) return ESP_ERR_NO_MEM;
    f.payload = b;
    esp_err_t e = httpd_ws_recv_frame(req, &f, f.len);
    if (e == ESP_OK && f.type == HTTPD_WS_TYPE_BINARY) return anim_stream_frame(b, f.len) == ESP_OK ? ESP_OK : ESP_OK;   // takes b
    free(b);
    return e;
}

void api_broadcast(const char *json)
{
    if (!s_server) return;
    size_t n = 8;
    int fds[8];
    if (httpd_get_client_list(s_server, &n, fds) != ESP_OK) return;
    for (size_t i = 0; i < n; i++) {
        if (httpd_ws_get_fd_info(s_server, fds[i]) != HTTPD_WS_CLIENT_WEBSOCKET) continue;
        httpd_ws_frame_t f = { .type = HTTPD_WS_TYPE_TEXT, .payload = (uint8_t *)json, .len = strlen(json) };
        httpd_ws_send_frame_async(s_server, fds[i], &f);
    }
}

esp_err_t api_start(api_status_cb_t status_cb)
{
    s_status_cb = status_cb;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 24;
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    cfg.stack_size = 8192;
    cfg.recv_wait_timeout = 20;
    cfg.send_wait_timeout = 20;
    cfg.lru_purge_enable = true;
    if (httpd_start(&s_server, &cfg) != ESP_OK) return ESP_FAIL;
    const httpd_uri_t uris[] = {
        { .uri = "/status",    .method = HTTP_GET,  .handler = h_status },
        { .uri = "/",          .method = HTTP_GET,  .handler = h_status },
        { .uri = "/image",     .method = HTTP_POST, .handler = h_image },
        { .uri = "/text",      .method = HTTP_POST, .handler = h_text },
        { .uri = "/fill",      .method = HTTP_POST, .handler = h_fill },
        { .uri = "/backlight", .method = HTTP_POST, .handler = h_backlight },
        { .uri = "/volume",    .method = HTTP_POST, .handler = h_volume },
        { .uri = "/audio",     .method = HTTP_POST, .handler = h_audio },
        { .uri = "/mic",       .method = HTTP_GET,  .handler = h_mic },
        { .uri = "/wifi",      .method = HTTP_POST, .handler = h_wifi },
        { .uri = "/reboot",    .method = HTTP_POST, .handler = h_reboot },
        { .uri = "/ota",       .method = HTTP_POST, .handler = h_ota },
        { .uri = "/ws",        .method = HTTP_GET,  .handler = h_ws, .is_websocket = true },
        { .uri = "/play",      .method = HTTP_POST, .handler = h_play },
        { .uri = "/stop",      .method = HTTP_POST, .handler = h_stop },
        { .uri = "/files/*",   .method = HTTP_GET,  .handler = h_files },
        { .uri = "/files/*",   .method = HTTP_PUT,  .handler = h_files },
        { .uri = "/files/*",   .method = HTTP_DELETE, .handler = h_files },
    };
    for (size_t i = 0; i < sizeof uris / sizeof uris[0]; i++) httpd_register_uri_handler(s_server, &uris[i]);
    ESP_LOGI(TAG, "HTTP API on port 80");
    return ESP_OK;
}
