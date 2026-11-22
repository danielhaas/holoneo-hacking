// Holoneo audio: ES8311 speaker codec + amp, ES7210 microphone ADC.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#define AUDIO_MIC_RATE 16000

esp_err_t audio_init(void);
// Plays interleaved 16-bit PCM (1 or 2 channels) at any common sample rate. Blocks until done.
esp_err_t audio_play(const int16_t *pcm, size_t frames, int channels, int rate);
// Records 16 kHz mono (mix of the two active mics). Blocks for frames/16000 seconds.
esp_err_t audio_record(int16_t *out, size_t frames);
// Speaker volume 0..100.
void audio_set_volume(int vol);
int audio_get_volume(void);
