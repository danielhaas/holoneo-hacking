#pragma once
#include "esp_err.h"
#include "cJSON.h"

// Called while building GET /status so the app can add its own fields.
typedef void (*api_status_cb_t)(cJSON *status);

esp_err_t api_start(api_status_cb_t status_cb);
// Sends a JSON text frame to every connected WebSocket client (/ws).
void api_broadcast(const char *json);
