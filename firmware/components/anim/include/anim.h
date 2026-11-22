// Animation player: plays MJPEG clips at their frame rate, double-buffered (the next frame is
// decoded while the current one is being sent to the panel).
//
// Supported clips:
//   - AVI with MJPEG video ('00dc'/'00db' chunks; audio chunks are skipped). The frame rate comes
//     from the AVI header. This is the format of the stock firmware's VIDEO/ folder.
//   - Concatenated JPEG files (".mjpeg"/".mjpg": FFD8..FFD9 one after another), fps given by caller.
// Frames are scaled to fit 320x240 (fastest when they are exactly 320x240).
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

esp_err_t anim_init(void);
// Plays a file (from /sd or /data). fps <= 0: use the clip's own rate (AVI) or 15.
esp_err_t anim_play_file(const char *path, bool loop, int fps);
// Plays a clip held in memory (PSRAM); the player takes ownership of buf and frees it.
esp_err_t anim_play_mem(uint8_t *buf, size_t len, bool loop, int fps);
void anim_stop(void);            // returns once playback has stopped
bool anim_playing(void);
// Live streaming: shows a JPEG frame as soon as possible (stops a running clip). Takes ownership
// of a heap buffer (freed after use); frames that can't be shown in time are dropped.
esp_err_t anim_stream_frame(uint8_t *jpg, size_t len);
// Same, but copies the frame first.
esp_err_t anim_show_frame(const uint8_t *jpg, size_t len);
// Last measured playback rate (frames per second, 0 if idle).
float anim_fps(void);
