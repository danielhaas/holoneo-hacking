#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "nvs.h"
#include "mdns.h"
#include "net.h"

static const char *TAG = "net";

#define BIT_CONNECTED BIT0
#define BIT_SETUP     BIT1
#define MAX_RETRIES   10

static EventGroupHandle_t s_ev;
static net_state_t s_state = NET_CONNECTING;
static char s_ip[16], s_ssid[33], s_token[17];
static int s_retries;
static esp_netif_t *s_sta, *s_ap;

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
    ESP_LOGW(TAG, "setup AP '%s' up (password = API token), http://192.168.4.1/", NET_SETUP_SSID);
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (s_ssid[0]) esp_wifi_connect();
        else start_setup_ap();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_state == NET_CONNECTED) ESP_LOGW(TAG, "disconnected");
        if (s_state != NET_SETUP_AP) s_state = NET_CONNECTING;
        s_ip[0] = 0;
        xEventGroupClearBits(s_ev, BIT_CONNECTED);
        if (++s_retries == MAX_RETRIES) start_setup_ap();   // keep retrying the station in the background
        vTaskDelay(pdMS_TO_TICKS(s_retries < MAX_RETRIES ? 1000 : 10000));
        if (s_ssid[0]) esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        snprintf(s_ip, sizeof s_ip, IPSTR, IP2STR(&e->ip_info.ip));
        s_retries = 0;
        if (s_state == NET_SETUP_AP) { esp_wifi_set_mode(WIFI_MODE_STA); ESP_LOGI(TAG, "setup AP off"); }
        s_state = NET_CONNECTED;
        xEventGroupSetBits(s_ev, BIT_CONNECTED);
        ESP_LOGI(TAG, "connected to '%s', IP %s, http://%s.local/", s_ssid, s_ip, NET_HOSTNAME);
    }
}

esp_err_t net_start(void)
{
    s_ev = xEventGroupCreate();
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

    // Station credentials persist in NVS (namespace nvs.net80211) — the stock firmware saved them
    // the same way, so an existing network is reused.
    wifi_config_t sta;
    esp_wifi_get_config(WIFI_IF_STA, &sta);
    strlcpy(s_ssid, (char *)sta.sta.ssid, sizeof s_ssid);
    ESP_LOGI(TAG, "saved station SSID: %s", s_ssid[0] ? "(set)" : "(none)");
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_ps(WIFI_PS_NONE);   // USB powered: favour latency over power
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start");

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

esp_err_t net_set_wifi(const char *ssid, const char *password)
{
    wifi_config_t sta = {0};
    strlcpy((char *)sta.sta.ssid, ssid, sizeof sta.sta.ssid);
    strlcpy((char *)sta.sta.password, password ? password : "", sizeof sta.sta.password);
    sta.sta.threshold.authmode = password && password[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    sta.sta.pmf_cfg.capable = true;
    strlcpy(s_ssid, ssid, sizeof s_ssid);
    s_retries = 0;
    esp_wifi_disconnect();
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &sta), TAG, "set config");
    return esp_wifi_connect();
}
