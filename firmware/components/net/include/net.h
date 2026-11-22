// Holoneo networking: Wi-Fi station (credentials from NVS, i.e. whatever was saved last —
// including by the stock firmware), fallback setup access point, mDNS, and the API token.
#pragma once
#include <stdbool.h>
#include "esp_err.h"

#define NET_HOSTNAME   "holoneo"
#define NET_SETUP_SSID "Holoneo-setup"

typedef enum { NET_CONNECTING, NET_CONNECTED, NET_SETUP_AP } net_state_t;

esp_err_t net_start(void);
net_state_t net_state(void);
const char *net_ip(void);          // "" if not connected
const char *net_ssid(void);        // configured station SSID ("" if none)
int net_rssi(void);
void net_ap_info(char *bssid, size_t n, int *channel);   // access point we are connected to
// Saves new station credentials and reconnects.
esp_err_t net_set_wifi(const char *ssid, const char *password);
// Random per-device API token (generated on first boot, stored in NVS). Also the setup-AP password.
const char *net_token(void);
// Blocks until connected or setup AP is up, or timeout. Returns the state.
net_state_t net_wait(int timeout_ms);
