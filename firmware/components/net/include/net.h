// Holoneo networking: up to NET_MAX_NETWORKS saved Wi-Fi networks (the strongest one in range is
// joined; in a mesh, the nearest access point), a WPA2 setup network with a captive portal as
// fallback, mDNS, and the API token.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#define NET_HOSTNAME     "holoneo"
#define NET_SETUP_SSID   "Holoneo-setup"
#define NET_MAX_NETWORKS 5

typedef enum { NET_CONNECTING, NET_CONNECTED, NET_SETUP_AP } net_state_t;

typedef struct { char ssid[33]; int rssi; bool secure; bool saved; } net_scan_result_t;

esp_err_t net_start(void);
net_state_t net_state(void);
const char *net_ip(void);          // "" if not connected; 192.168.4.1 while the setup network is up
const char *net_ssid(void);        // network currently used / being joined
int net_rssi(void);
void net_ap_info(char *bssid, size_t n, int *channel);   // access point we are connected to

// Saves (or updates) a network, makes it the first choice and switches to it.
esp_err_t net_set_wifi(const char *ssid, const char *password);
esp_err_t net_forget_wifi(const char *ssid);
// Fills up to max saved SSIDs (most recently added first); returns the count.
int net_saved(char ssids[][33], int max);
// Scans for networks (one entry per SSID, strongest access point). Blocks ~1-2 s.
int net_scan(net_scan_result_t *out, int max);

// Random per-device API token (generated on first boot, stored in NVS). Also the setup-network password.
const char *net_token(void);
// Blocks until connected or the setup network is up, or timeout. Returns the state.
net_state_t net_wait(int timeout_ms);
// True if the socket's peer is on our setup network (it already proved it knows the token).
bool net_is_setup_client(int sockfd);
