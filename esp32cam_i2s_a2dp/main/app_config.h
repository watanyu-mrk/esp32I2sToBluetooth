// Compile time configuration shared by the whole application.
// Everything that is user tunable comes from main/Kconfig.projbuild (menuconfig).
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "sdkconfig.h"

#define APP_AUDIO_CHANNELS 2
#define APP_BYTES_PER_FRAME (APP_AUDIO_CHANNELS * sizeof(int16_t))

// ---------------------------------------------------------------------------
// Bluetooth
// ---------------------------------------------------------------------------
#define APP_BT_DEVICE_NAME CONFIG_APP_BT_DEVICE_NAME
#define APP_BT_TARGET_NAME CONFIG_APP_BT_TARGET_NAME
#define APP_BT_RECONNECT_TRIES 1000

// ---------------------------------------------------------------------------
// Sample rates
// ---------------------------------------------------------------------------
#if CONFIG_APP_OUTPUT_RATE_48000
#define APP_OUTPUT_SAMPLE_RATE 48000
#else
#define APP_OUTPUT_SAMPLE_RATE 44100
#endif

#if CONFIG_APP_INPUT_RATE_48000
#define APP_INPUT_SAMPLE_RATE 48000
#else
#define APP_INPUT_SAMPLE_RATE 44100
#endif

// ---------------------------------------------------------------------------
// I2S receiver (slave: the ESP32-S3 generates BCLK/WS)
// ---------------------------------------------------------------------------
#define APP_I2S_BCLK_GPIO CONFIG_APP_I2S_BCLK_GPIO
#define APP_I2S_WS_GPIO CONFIG_APP_I2S_WS_GPIO
#define APP_I2S_DATA_GPIO CONFIG_APP_I2S_DATA_GPIO
#define APP_I2S_PCM_SHIFT_BITS CONFIG_APP_I2S_PCM_SHIFT_BITS

#define APP_I2S_CHUNK_FRAMES 256
#define APP_I2S_READ_TIMEOUT_MS 100
#define APP_I2S_TASK_STACK 4096
#define APP_I2S_TASK_PRIO 5
#define APP_I2S_TASK_CORE 1

// ---------------------------------------------------------------------------
// Audio buffer
// ---------------------------------------------------------------------------
#define APP_BUFFER_MS CONFIG_APP_AUDIO_BUFFER_MS
#define APP_BUFFER_BYTES \
  ((size_t)APP_OUTPUT_SAMPLE_RATE * APP_BYTES_PER_FRAME * APP_BUFFER_MS / 1000)

// ---------------------------------------------------------------------------
// Test tone (CONFIG_APP_TEST_TONE): 10 ms chunks so it can be paced by a
// 1 kHz FreeRTOS tick without cumulative drift.
// ---------------------------------------------------------------------------
#define APP_TONE_FREQ_HZ 1000
#define APP_TONE_AMPLITUDE 8000
#define APP_TONE_CHUNK_FRAMES (APP_OUTPUT_SAMPLE_RATE / 100)

// Returning false stops the producers from queueing audio (nothing is
// streaming over Bluetooth yet, or the speaker disconnected).
#ifdef __cplusplus
extern "C" {
#endif
typedef bool (*audio_gate_t)(void);
#ifdef __cplusplus
}
#endif
