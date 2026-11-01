// Compile time configuration shared by the whole application.
// Everything that is user tunable comes from main/Kconfig.projbuild (menuconfig).
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sdkconfig.h"

#define APP_AUDIO_CHANNELS 2
#define APP_SAMPLE_BYTES 2
#define APP_BYTES_PER_FRAME (APP_AUDIO_CHANNELS * APP_SAMPLE_BYTES)

// Bluedroid's SBC encoder is fed with a fixed 44100 Hz PCM stream
// (btc_a2dp_source.c: media_feeding.cfg.pcm.sampling_freq = 44100), so every
// station is converted to this rate before it is queued for Bluetooth.
#define APP_OUTPUT_SAMPLE_RATE 44100

// ---------------------------------------------------------------------------
// Bluetooth
// ---------------------------------------------------------------------------
#define APP_BT_DEVICE_NAME CONFIG_APP_BT_DEVICE_NAME
#define APP_BT_TARGET_NAME CONFIG_APP_BT_TARGET_NAME
#define APP_BT_PIN_CODE CONFIG_APP_BT_PIN_CODE
#define APP_BT_RECONNECT_TRIES 1000

#if CONFIG_APP_BT_SSP
#define APP_BT_SSP_ENABLED 1
#else
#define APP_BT_SSP_ENABLED 0
#endif

// ---------------------------------------------------------------------------
// Wi-Fi
// ---------------------------------------------------------------------------
#define APP_WIFI_SSID CONFIG_APP_WIFI_SSID
#define APP_WIFI_PASSWORD CONFIG_APP_WIFI_PASSWORD

// Setup access point, raised when the configured network cannot be reached.
#define APP_WIFI_AP_SSID "ESP32-Radio-Setup"
#define APP_WIFI_AP_PASSWORD "esp32radio"  // WPA2 needs at least 8 characters
#define APP_WIFI_AP_CHANNEL 6
#define APP_WIFI_AP_MAX_CLIENTS 2
// Microseconds, the unit esp_timer_get_time() returns.
#define APP_WIFI_JOIN_TIMEOUT_US \
  ((int64_t)CONFIG_APP_WIFI_JOIN_TIMEOUT_S * 1000 * 1000)

// ---------------------------------------------------------------------------
// Radio stream
// ---------------------------------------------------------------------------
#define APP_STATION_URL CONFIG_APP_STATION_URL
#define APP_STATION_NAME CONFIG_APP_STATION_NAME

// Bytes pulled from the socket per read. Bigger reads cut the number of HTTP
// calls, smaller ones react faster to a station switch.
#define APP_RADIO_READ_CHUNK 1024
// MP3 bytes waiting to be decoded. Must hold at least one full frame, and a
// little slack so a read does not have to be split.
#define APP_RADIO_INPUT_BYTES 6144
// Largest MPEG 1/2/2.5 Layer III frame (144 * 320 kbps / 32 kHz + padding).
// The decoder is only called once a full frame is buffered, so it never
// reports a partial frame in the middle of the stream.
#define APP_MP3_MAX_FRAME_BYTES 1441
// Decode scratch space: 2 granules x 576 samples x 2 channels, with slack.
#define APP_PCM_FRAMES 2304

#define APP_RADIO_TASK_STACK 9216
#define APP_RADIO_TASK_PRIO 5
// Core 1: the Bluetooth host and the Wi-Fi driver both live on core 0.
#define APP_RADIO_TASK_CORE 1

#define APP_RADIO_RETRY_MIN_MS 2000
#define APP_RADIO_RETRY_MAX_MS 15000

// ---------------------------------------------------------------------------
// Audio buffer (decoded PCM waiting for the Bluetooth stack)
// ---------------------------------------------------------------------------
#define APP_BUFFER_MS CONFIG_APP_AUDIO_BUFFER_MS
#define APP_BUFFER_BYTES \
  ((size_t)APP_OUTPUT_SAMPLE_RATE * APP_BYTES_PER_FRAME * APP_BUFFER_MS / 1000)

// ---------------------------------------------------------------------------
// Config page
// ---------------------------------------------------------------------------
// The Wi-Fi AP plus the HTTP server need roughly 30 KB.
#define APP_PORTAL_MIN_HEAP_BYTES (28 * 1024)
