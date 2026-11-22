// Holoneo display: 320x240 landscape (as seen in the reflection), RGB565.
// Drawing goes to a PSRAM framebuffer; display_flush() sends it to the panel.
// All functions are thread-safe (one internal mutex); call display_lock()/unlock()
// around multi-step drawing to make it appear atomically.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

#define DISPLAY_W 320
#define DISPLAY_H 240

esp_err_t display_init(void);
void display_backlight(bool on);

void display_lock(void);
void display_unlock(void);

// Colours are given as 0xRRGGBB.
void display_fill(uint32_t rgb);
void display_rect(int x, int y, int w, int h, uint32_t rgb);
// Draws ASCII text at pixel position (top-left), scale 1..4. '\n' starts a new line.
void display_text(int x, int y, const char *s, uint32_t rgb, int scale);
// Draws text centred on the screen, word-wrapped to the screen width.
void display_text_centered(const char *s, uint32_t rgb, int scale);
// Decodes a baseline JPEG, scales it down (1/2, 1/4, 1/8) until it fits and centres it on black.
esp_err_t display_jpeg(const uint8_t *jpg, size_t len);
// Copies the framebuffer to the panel.
void display_flush(void);

// Save / restore the whole framebuffer (e.g. to show an overlay temporarily).
void display_snapshot(void);
void display_restore(void);

int display_text_cols(int scale);
int display_text_rows(int scale);
