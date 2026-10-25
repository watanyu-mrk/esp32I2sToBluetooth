// I2S slave receiver: the ESP32-S3 is the only clock master (BCLK/WS), this
// board just samples DIN. Each 32-bit slot is converted to PCM16 and queued
// in the audio buffer.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "app_config.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  int bclk_gpio;
  int ws_gpio;
  int data_gpio;
  uint32_t sample_rate;  // incoming rate (slave: descriptive, not generated)
  uint32_t output_rate;  // rate the A2DP stream consumes (resampler target)
  int shift_bits;        // right shift converting a 32-bit slot to PCM16
  audio_gate_t gate;     // optional: return false to drop input
  int core_id;
} i2s_receiver_config_t;

typedef struct {
  uint64_t bytes_read;
  uint32_t read_errors;
  uint32_t timeouts;
  uint32_t gate_dropped_frames;
  uint32_t resampled_frames;
} i2s_receiver_stats_t;

bool i2s_receiver_start(const i2s_receiver_config_t *config);
void i2s_receiver_stop(void);
i2s_receiver_stats_t i2s_receiver_get_stats(void);

#ifdef __cplusplus
}
#endif
