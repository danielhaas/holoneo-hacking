#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_wifi.h"
#include "esp_mac.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "lwip/sockets.h"
#include "nvs.h"
#include "mdns.h"
#include "net.h"

static const char *TAG = "net";

#define BIT_CONNECTED    BIT0
#define BIT_SETUP        BIT1
#define BIT_DISCONNECTED BIT2
#define BIT_RESELECT     BIT3

static EventGroupHandle_t s_ev;
static SemaphoreHandle_t s_scan_lock;
static volatile net_state_t s_state = NET_CONNECTING;
static char s_ip[16], s_ssid[33], s_token[17];
static esp_netif_t *s_sta, *s_ap;
static char s_prefer[33];   // network to try first after net_set_wifi()

// ------------------------------------------------------------------ saved networks
typedef struct { char ssid[33]; char pass[65]; } saved_t;
static saved_t s_nets[NET_MAX_NETWORKS];
static int s_nnets;

static void nets_save(void)
{
    nvs_handle_t h;
    if (nvs_open("wifi", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, "nets", s_nets, sizeof(saved_t) * s_nnets);
    nvs_set_u8(h, "count", s_nnets);
    nvs_commit(h);
    nvs_close(h);
}

static void nets_load(void)
{
    nvs_handle_t h;
    s_nnets = 0;
    if (nvs_open("wifi", NVS_READONLY, &h) == ESP_OK) {
        uint8_t n = 0;
        size_t len = sizeof s_nets;
        if (nvs_get_u8(h, "count", &n) == ESP_OK && n <= NET_MAX_NETWORKS &&
            nvs_get_blob(h, "nets", s_nets, &len) == ESP_OK && len == sizeof(saved_t) * n)
            s_nnets = n;
        nvs_close(h);
    }
    if (s_nnets == 0) {
        // First boot with this firmware: adopt the network ESP-IDF has stored (e.g. by the stock firmware).
        wifi_config_t old;
        if (esp_wifi_get_config(WIFI_IF_STA, &old) == ESP_OK && old.sta.ssid[0]) {
            strlcpy(s_nets[0].ssid, (char *)old.sta.ssid, sizeof s_nets[0].ssid);
            strlcpy(s_nets[0].pass, (char *)old.sta.password, sizeof s_nets[0].pass);
            s_nnets = 1;
            nets_save();
            ESP_LOGI(TAG, "adopted the previously saved network");
        }
    }
    ESP_LOGI(TAG, "%d saved network(s)", s_nnets);
}

static int nets_find(const char *ssid)
{
    for (int i = 0; i < s_nnets; i++) if (!strcmp(s_nets[i].ssid, ssid)) return i;
    return -1;
}

esp_err_t net_set_wifi(const char *ssid, const char *password)
{
    if (!ssid || !ssid[0] || strlen(ssid) > 32 || (password && strlen(password) > 64)) return ESP_ERR_INVALID_ARG;
    int i = nets_find(ssid);
    if (i < 0) {
        if (s_nnets == NET_MAX_NETWORKS) s_nnets--;   // full: drop the last (least recently added)
        i = s_nnets++;
    }
    saved_t n = {0};
    strlcpy(n.ssid, ssid, sizeof n.ssid);
    strlcpy(n.pass, password ? password : "", sizeof n.pass);
    memmove(&s_nets[1], &s_nets[0], sizeof(saved_t) * i);   // most recently added first
    s_nets[0] = n;
    nets_save();
    strlcpy(s_prefer, ssid, sizeof s_prefer);
    ESP_LOGI(TAG, "saved network '%s' (%d stored), switching to it", ssid, s_nnets);
    xEventGroupSetBits(s_ev, BIT_RESELECT);
    return ESP_OK;
}

esp_err_t net_forget_wifi(const char *ssid)
{
    int i = nets_find(ssid);
    if (i < 0) return ESP_ERR_NOT_FOUND;
    memmove(&s_nets[i], &s_nets[i + 1], sizeof(saved_t) * (s_nnets - i - 1));
    s_nnets--;
    nets_save();
    ESP_LOGI(TAG, "forgot network '%s'", ssid);
    return ESP_OK;
}

int net_saved(char ssids[][33], int max)
{
    int n = s_nnets < max ? s_nnets : max;
    for (int i = 0; i < n; i++) strlcpy(ssids[i], s_nets[i].ssid, 33);
    return n;
}

// ------------------------------------------------------------------ token, status
static void load_token(void)
{
    nvs_handle_t h;
    size_t len = sizeof s_token;
    if (nvs_open("holo", NVS_READWRITE, &h) != ESP_OK) { strcpy(s_token, "holoneo-setup"); return; }
    if (nvs_get_str(h, "token", s_token, &len) != ESP_OK || strlen(s_token) < 8) {
        static const char alphabet[] = "abcdefghijkmnpqrstuvwxyz23456789";   // no 0/o/1/l
        for (int i = 0; i < 12; i++) s_token[i] = alphabet[esp_random() % (sizeof alphabet - 1)];
        s_token[12] = 0;
        nvs_set_str(h, "token", s_token);
        nvs_commit(h);
        ESP_LOGW(TAG, "generated new API token");
    }
    nvs_close(h);
}

const char *net_token(void) { return s_token; }
net_state_t net_state(void) { return s_state; }
const char *net_ip(void) { return s_ip; }
const char *net_ssid(void) { return s_ssid; }

int net_rssi(void)
{
    wifi_ap_record_t ap;
    return esp_wifi_sta_get_ap_info(&ap) == ESP_OK ? ap.rssi : 0;
}

void net_ap_info(char *bssid, size_t n, int *channel)
{
    wifi_ap_record_t ap;
    *channel = 0;
    bssid[0] = 0;
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) return;
    snprintf(bssid, n, MACSTR, MAC2STR(ap.bssid));
    *channel = ap.primary;
}

// ------------------------------------------------------------------ scanning
int net_scan(net_scan_result_t *out, int max)
{
    xSemaphoreTake(s_scan_lock, portMAX_DELAY);
    wifi_scan_config_t sc = { .show_hidden = false, .scan_type = WIFI_SCAN_TYPE_ACTIVE,
                              .scan_time.active = { .min = 60, .max = 120 } };
    int n = 0;
    if (esp_wifi_scan_start(&sc, true) == ESP_OK) {
        uint16_t cnt = 32;
        wifi_ap_record_t *recs = calloc(cnt, sizeof *recs);
        if (recs && esp_wifi_scan_get_ap_records(&cnt, recs) == ESP_OK) {
            for (int i = 0; i < cnt; i++) {
                if (!recs[i].ssid[0]) continue;
                int k;   // one entry per SSID (mesh: keep the strongest access point)
                for (k = 0; k < n && strcmp(out[k].ssid, (char *)recs[i].ssid); k++) {}
                if (k < n) { if (recs[i].rssi > out[k].rssi) out[k].rssi = recs[i].rssi; continue; }
                if (n == max) continue;
                strlcpy(out[n].ssid, (char *)recs[i].ssid, sizeof out[n].ssid);
                out[n].rssi = recs[i].rssi;
                out[n].secure = recs[i].authmode != WIFI_AUTH_OPEN;
                out[n].saved = nets_find(out[n].ssid) >= 0;
                n++;
            }
        }
        free(recs);
    }
    xSemaphoreGive(s_scan_lock);
    return n;
}

// ------------------------------------------------------------------ setup access point + captive DNS
static void dns_task(void *arg)
{
    // Answers every DNS query with 192.168.4.1 so phones open the setup page by themselves.
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(53), .sin_addr.s_addr = htonl(INADDR_ANY) };
    if (fd < 0 || bind(fd, (struct sockaddr *)&a, sizeof a) < 0) { ESP_LOGE(TAG, "dns bind failed"); vTaskDelete(NULL); }
    uint8_t buf[512];
    for (;;) {
        struct sockaddr_in from;
        socklen_t fl = sizeof from;
        int n = recvfrom(fd, buf, sizeof buf - 16, 0, (struct sockaddr *)&from, &fl);
        if (n < 12 || s_state != NET_SETUP_AP) continue;
        buf[2] = 0x81; buf[3] = 0x80;          // response, recursion available, no error
        buf[6] = 0; buf[7] = 1;                // one answer
        buf[8] = buf[9] = buf[10] = buf[11] = 0;
        static const uint8_t ans[] = { 0xC0, 0x0C, 0, 1, 0, 1, 0, 0, 0, 60, 0, 4, 192, 168, 4, 1 };
        memcpy(buf + n, ans, sizeof ans);
        sendto(fd, buf, n + sizeof ans, 0, (struct sockaddr *)&from, fl);
    }
}

static void start_setup_ap(void)
{
    if (s_state == NET_SETUP_AP) return;
    wifi_config_t ap = { .ap = { .ssid = NET_SETUP_SSID, .ssid_len = strlen(NET_SETUP_SSID),
                                 .channel = 6, .max_connection = 2, .authmode = WIFI_AUTH_WPA2_PSK } };
    strlcpy((char *)ap.ap.password, s_token, sizeof ap.ap.password);
    esp_wifi_set_mode(WIFI_MODE_APSTA);
    esp_wifi_set_config(WIFI_IF_AP, &ap);
    s_state = NET_SETUP_AP;
    strcpy(s_ip, "192.168.4.1");
    xEventGroupSetBits(s_ev, BIT_SETUP);
    static bool dns_started;
    if (!dns_started) { dns_started = true; xTaskCreate(dns_task, "dns", 3072, NULL, 4, NULL); }
    ESP_LOGW(TAG, "setup network '%s' up (password = API token), http://192.168.4.1/", NET_SETUP_SSID);
}

static void stop_setup_ap(void)
{
    if (s_state != NET_SETUP_AP) return;
    xEventGroupClearBits(s_ev, BIT_SETUP);
    esp_wifi_set_mode(WIFI_MODE_STA);
    ESP_LOGI(TAG, "setup network off");
}

// ------------------------------------------------------------------ connection management
// With a mesh (several APs with the same SSID) the default fast scan joins the first AP that
// answers, often a far one, and ESP-IDF never roams on its own. Scan all channels, join the
// strongest, and let the mesh steer us (802.11k neighbour reports, 802.11v BSS transition).
static void prefer_strongest_ap(wifi_config_t *sta)
{
    sta->sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    sta->sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    sta->sta.bssid_set = false;
    sta->sta.rm_enabled = 1;
    sta->sta.btm_enabled = 1;
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_state == NET_CONNECTED) { ESP_LOGW(TAG, "disconnected from '%s'", s_ssid); s_state = NET_CONNECTING; }
        s_ip[0] = 0;
        xEventGroupClearBits(s_ev, BIT_CONNECTED);
        xEventGroupSetBits(s_ev, BIT_DISCONNECTED);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        snprintf(s_ip, sizeof s_ip, IPSTR, IP2STR(&e->ip_info.ip));
        stop_setup_ap();
        s_state = NET_CONNECTED;
        xEventGroupSetBits(s_ev, BIT_CONNECTED);
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK)
            ESP_LOGI(TAG, "access point " MACSTR ", channel %d, RSSI %d dBm", MAC2STR(ap.bssid), ap.primary, ap.rssi);
        ESP_LOGI(TAG, "connected to '%s', IP %s, http://%s.local/", s_ssid, s_ip, NET_HOSTNAME);
    }
}

// Picks the saved network to join: the preferred one if visible, else the strongest visible.
static int pick_network(void)
{
    net_scan_result_t *seen = calloc(24, sizeof *seen);
    if (!seen) return -1;
    int n = net_scan(seen, 24), best = -1, best_rssi = -1000;
    for (int i = 0; i < n; i++) {
        int k = nets_find(seen[i].ssid);
        if (k < 0) continue;
        if (s_prefer[0] && !strcmp(s_prefer, seen[i].ssid)) { best = k; break; }
        if (seen[i].rssi > best_rssi) { best = k; best_rssi = seen[i].rssi; }
    }
    free(seen);
    s_prefer[0] = 0;
    return best;
}

static bool try_connect(int k)
{
    wifi_config_t sta = {0};
    strlcpy((char *)sta.sta.ssid, s_nets[k].ssid, sizeof sta.sta.ssid);
    strlcpy((char *)sta.sta.password, s_nets[k].pass, sizeof sta.sta.password);
    sta.sta.threshold.authmode = s_nets[k].pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    sta.sta.pmf_cfg.capable = true;
    prefer_strongest_ap(&sta);
    strlcpy(s_ssid, s_nets[k].ssid, sizeof s_ssid);
    ESP_LOGI(TAG, "connecting to '%s'", s_ssid);
    xEventGroupClearBits(s_ev, BIT_DISCONNECTED | BIT_CONNECTED);
    esp_wifi_set_config(WIFI_IF_STA, &sta);
    esp_wifi_connect();
    EventBits_t b = xEventGroupWaitBits(s_ev, BIT_CONNECTED | BIT_DISCONNECTED, pdFALSE, pdFALSE, pdMS_TO_TICKS(20000));
    return b & BIT_CONNECTED;
}

static void wifi_task(void *arg)
{
    int misses = 0;
    for (;;) {
        if (s_state == NET_CONNECTED) {
            // stay until we lose the connection or a new network is added
            EventBits_t b = xEventGroupWaitBits(s_ev, BIT_DISCONNECTED | BIT_RESELECT, pdTRUE, pdFALSE, portMAX_DELAY);
            if (b & BIT_RESELECT) esp_wifi_disconnect();
            if (b & BIT_DISCONNECTED) vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        xEventGroupClearBits(s_ev, BIT_RESELECT);
        int k = s_nnets ? pick_network() : -1;
        if (k >= 0 && (try_connect(k) || try_connect(k))) { misses = 0; continue; }
        // Nothing known in range (or it failed): open the setup network, keep looking in the background.
        if (++misses >= 2 || s_nnets == 0) start_setup_ap();
        xEventGroupWaitBits(s_ev, BIT_RESELECT, pdFALSE, pdFALSE, pdMS_TO_TICKS(s_state == NET_SETUP_AP ? 20000 : 3000));
    }
}

esp_err_t net_start(void)
{
    s_ev = xEventGroupCreate();
    s_scan_lock = xSemaphoreCreateMutex();
    load_token();
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "event loop");
    s_sta = esp_netif_create_default_wifi_sta();
    s_ap = esp_netif_create_default_wifi_ap();
    esp_netif_set_hostname(s_sta, NET_HOSTNAME);
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "wifi init");
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL);
    nets_load();
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_ps(WIFI_PS_NONE);   // USB powered: favour latency over power
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start");
    xTaskCreate(wifi_task, "wifi_mgr", 4096, NULL, 5, NULL);

    if (mdns_init() == ESP_OK) {
        mdns_hostname_set(NET_HOSTNAME);
        mdns_instance_name_set("Holoneo");
        mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    }
    return ESP_OK;
}

net_state_t net_wait(int timeout_ms)
{
    xEventGroupWaitBits(s_ev, BIT_CONNECTED | BIT_SETUP, pdFALSE, pdFALSE, pdMS_TO_TICKS(timeout_ms));
    return s_state;
}

bool net_is_setup_client(int sockfd)
{
    if (s_state != NET_SETUP_AP) return false;
    struct sockaddr_storage ss;
    socklen_t len = sizeof ss;
    if (getpeername(sockfd, (struct sockaddr *)&ss, &len) != 0) return false;
    uint32_t ip = 0;
    if (ss.ss_family == AF_INET) ip = ntohl(((struct sockaddr_in *)&ss)->sin_addr.s_addr);
#if LWIP_IPV6
    else if (ss.ss_family == AF_INET6) {   // IPv4-mapped IPv6 address
        struct sockaddr_in6 *s6 = (struct sockaddr_in6 *)&ss;
        ip = ntohl(s6->sin6_addr.un.u32_addr[3]);
    }
#endif
    return (ip & 0xFFFFFF00) == 0xC0A80400;   // 192.168.4.0/24 = our setup network
}
