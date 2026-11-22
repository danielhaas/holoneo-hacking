// Simple gestures from the Holoneo's EPL2219 IR proximity/gesture sensor (I2C 0x41).
//
// The sensor delivers two IR channels (A, B) that see almost the same thing, so swipe
// direction (left/right) can't be told apart reliably. What works:
//   WAVE  - a short swipe in any direction
//   HOVER - hand held in front of the sensor (long, strong signal)
//   UP    - bottom -> top swipe (experimental: channel A clearly stronger than B, early peak)
// See docs/hardware.md for the measurements behind this.
#pragma once
#include "driver/i2c_master.h"

typedef enum { GESTURE_WAVE, GESTURE_HOVER, GESTURE_UP } gesture_t;

typedef void (*gesture_cb_t)(gesture_t g, void *ctx);

// Starts a background task polling the sensor every 5 ms on the given bus.
esp_err_t gesture_start(i2c_master_bus_handle_t bus, gesture_cb_t cb, void *ctx);

const char *gesture_name(gesture_t g);
