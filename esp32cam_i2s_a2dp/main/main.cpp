// ESP32-CAM (original ESP32, classic Bluetooth) audio bridge:
//
//   ESP32-S3 Internet Radio --I2S--> ESP32-CAM --A2DP--> Bluetooth speaker
//
// The ESP32-S3 is the only clock master; this board receives, converts the
// 32-bit I2S slots to PCM16 stereo, buffers them, and lets the A2DP source
// stream pull from that buffer.

#include <inttypes.h>
#include <string.h>

#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#if CONFIG_SPIRAM
#include "esp_psram.h"
#endif

#include "a2dp_source.h"
#include "app_config.h"
#include "app_settings.h"
#include "audio_buffer.h"
#include "config_portal.h"
#include "i2s_receiver.h"
#if CONFIG_APP_PIN_CHECK && !CONFIG_APP_TEST_TONE
#include "pin_activity.h"
#endif
#if CONFIG_APP_LOOPBACK_TEST
#include "loopback_test.h"
#endif
#if CONFIG_APP_TEST_TONE
#include "tone_source.h"
#endif

static const char *TAG = "app";

// The config portal (Wi-Fi AP + web page) is the fallback, never the default
// path: Wi-Fi and Bluetooth Classic share the radio and the heap, so raising
// the AP while the A2DP source is still hunting for its speaker costs the
// stream both. The board boots straight into Bluetooth and only after
// APP_CONFIG_PORTAL_DELAY_MS without a connected speaker does it shut the
// Bluetooth stack down - releasing its memory to the Wi-Fi stack - and raise
// the portal. There is no way back from that without a restart, which is
// exactly what saving the page does.
#define PORTAL_RETRY_MS 10000
// The Wi-Fi AP plus the HTTP server need roughly 30 KB.
#define PORTAL_MIN_HEAP_BYTES (32 * 1024)

static int64_t s_disconnected_since_us;
static bool s_bt_released;

// Nothing is streaming once Bluetooth is gone (the audio gate is closed), so
// the ring buffer is dead weight. Its ~64 KB go back to the system and stay
// there: the Bluetooth stack that would fill it cannot be restarted.
static void release_audio_buffer(void) {
  audio_buffer_deinit();
  ESP_LOGI(TAG, "audio buffer released for the config portal (%u bytes free)",
           (unsigned)esp_get_free_heap_size());
}

static void service_config_portal(void) {
  if (config_portal_is_running()) return;

  if (a2dp_source_is_active()) {
    s_disconnected_since_us = 0;
    return;
  }

  int64_t now = esp_timer_get_time();
  if (s_disconnected_since_us == 0) s_disconnected_since_us = now;
  if (now - s_disconnected_since_us <
      (int64_t)APP_CONFIG_PORTAL_DELAY_MS * 1000) {
    return;
  }

  // Retry slowly: a failing attempt should not be repeated every tick.
  static int64_t last_attempt_us;
  if (last_attempt_us != 0 && now - last_attempt_us < PORTAL_RETRY_MS * 1000) {
    return;
  }
  last_attempt_us = now;

  // Bluetooth first, Wi-Fi second: the Wi-Fi stack needs the memory the
  // Bluetooth controller is holding, and it needs the radio without a coex
  // partner still trying to discover a speaker.
  if (!s_bt_released) {
    ESP_LOGW(TAG, "no speaker after %d s: switching to the config portal",
             CONFIG_APP_PORTAL_DELAY_S);
    a2dp_source_stop();
    s_bt_released = true;
    release_audio_buffer();
  }

  if (esp_get_free_heap_size() < PORTAL_MIN_HEAP_BYTES) {
    ESP_LOGW(TAG, "not enough free heap for the config portal (%u bytes)",
             (unsigned)esp_get_free_heap_size());
    return;
  }

  if (!config_portal_start()) {
    ESP_LOGE(TAG, "config portal start failed, retrying");
  }
}

static void log_environment(void) {
  esp_chip_info_t chip_info;
  esp_chip_info(&chip_info);

  uint32_t flash_size = 0;
  esp_flash_get_size(NULL, &flash_size);

  uint8_t mac[6] = {0};
  esp_read_mac(mac, ESP_MAC_BT);

  ESP_LOGI(TAG, "ESP32-CAM I2S -> A2DP source bridge");
  ESP_LOGI(TAG, "ESP-IDF %s | %d core(s) | flash %" PRIu32 " MB",
           esp_get_idf_version(), chip_info.cores, flash_size / (1024 * 1024));
  const app_settings_t *settings = app_settings_get();

  ESP_LOGI(TAG, "BT MAC %02x:%02x:%02x:%02x:%02x:%02x | local name '%s'",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
           settings->local_name);
#if CONFIG_SPIRAM
  ESP_LOGI(TAG, "PSRAM %u bytes (audio buffers stay in internal RAM)",
           (unsigned)esp_psram_get_size());
#endif
  ESP_LOGI(TAG, "A2DP %" PRIu32 " Hz, I2S input %" PRIu32 " Hz",
           settings->output_rate, settings->input_rate);
  if (settings->input_rate != settings->output_rate) {
    ESP_LOGW(TAG, "input and output rates differ: linear resampling is active");
  }
}

static void log_status(void) {
  static i2s_receiver_stats_t previous;
  static int64_t previous_us;
  static bool first = true;

  i2s_receiver_stats_t rx = i2s_receiver_get_stats();
  audio_buffer_stats_t buffer = audio_buffer_get_stats();
  int64_t now_us = esp_timer_get_time();

  if (first) {
    previous = rx;
    previous_us = now_us;
    first = false;
    return;
  }

  double elapsed = (double)(now_us - previous_us) / 1000000.0;
  uint64_t delta_bytes = rx.bytes_read - previous.bytes_read;
  unsigned bytes_per_second =
      elapsed > 0.0 ? (unsigned)(delta_bytes / elapsed) : 0;
  unsigned percent =
      buffer.capacity_frames > 0
          ? (unsigned)(buffer.available_frames * 100 / buffer.capacity_frames)
          : 0;

  ESP_LOGI(TAG,
           "BT=%-10s | I2S %6u B/s | buf %3u%% (%u/%u frames) | "
           "drop %" PRIu32 " fr, underflow %" PRIu32 " fr | "
           "timeouts %" PRIu32 ", errors %" PRIu32 " | heap %u",
           a2dp_source_state(), bytes_per_second, percent,
           (unsigned)buffer.available_frames, (unsigned)buffer.capacity_frames,
           buffer.dropped_frames, buffer.underflow_frames, rx.timeouts,
           rx.read_errors, (unsigned)esp_get_free_heap_size());

  previous = rx;
  previous_us = now_us;
}

#if CONFIG_APP_PIN_CHECK && !CONFIG_APP_TEST_TONE
// The receiver is an I2S slave, so a bus that never gets a clock and a bus
// wired to the wrong pins both look like "no audio" in the status log. Sample
// the three lines once, before the I2S driver takes them over, and report
// which of them are actually alive.
static void check_i2s_input_pins(void) {
  const int gpios[] = {APP_I2S_BCLK_GPIO, APP_I2S_WS_GPIO, APP_I2S_DATA_GPIO};
  static const char *const names[] = {"BCLK", "WS", "DATA"};
  const size_t count = sizeof(gpios) / sizeof(gpios[0]);
  pin_activity_t activity[count];

  if (!pin_activity_measure(gpios, count, APP_PIN_CHECK_WINDOW_MS, activity)) {
    ESP_LOGW(TAG, "I2S pin activity check could not run");
    return;
  }

  bool toggling[count];
  for (size_t i = 0; i < count; i++) {
    toggling[i] = pin_activity_is_toggling(&activity[i]);
    ESP_LOGI(TAG, "%s (GPIO%2d): %s, %" PRIu32 " edges in %u ms", names[i],
             activity[i].gpio,
             toggling[i] ? "toggling"
                         : (activity[i].last_level ? "static HIGH"
                                                   : "static LOW"),
             activity[i].transitions, (unsigned)activity[i].window_ms);
  }

  if (!toggling[0]) {
    ESP_LOGW(TAG,
             "no BCLK: the ESP32-S3 is not clocking the bus, so the receiver "
             "will only time out. Expected S3 BCLK->GPIO%d, WS->GPIO%d, "
             "DATA->GPIO%d and a shared GND, with the S3 playing audio",
             APP_I2S_BCLK_GPIO, APP_I2S_WS_GPIO, APP_I2S_DATA_GPIO);
  } else if (!toggling[1]) {
    ESP_LOGW(TAG,
             "BCLK is running but WS is idle: the S3 clocks without frames "
             "(check the WS wire on GPIO%d)",
             APP_I2S_WS_GPIO);
  } else if (!toggling[2]) {
    ESP_LOGI(TAG,
             "clocks are running, DIN on GPIO%d stays at a fixed level "
             "(digital silence, or the DATA wire is missing)",
             APP_I2S_DATA_GPIO);
  } else {
    ESP_LOGI(TAG, "I2S input looks alive: clocks and data are toggling");
  }
}
#endif

extern "C" void app_main(void) {
  app_settings_load();
  log_environment();

  const app_settings_t *settings = app_settings_get();
  size_t buffer_bytes = (size_t)settings->output_rate * APP_BYTES_PER_FRAME *
                        settings->buffer_ms / 1000;
  if (!audio_buffer_init(buffer_bytes)) {
    ESP_LOGE(TAG, "audio buffer init failed - stopping");
    return;
  }
  ESP_LOGI(TAG, "audio buffer: %u frames (%u ms)",
           (unsigned)audio_buffer_get_stats().capacity_frames,
           (unsigned)(audio_buffer_get_stats().capacity_frames * 1000 /
                      settings->output_rate));

#if CONFIG_APP_PIN_CHECK && !CONFIG_APP_TEST_TONE
  check_i2s_input_pins();
#endif

#if CONFIG_APP_LOOPBACK_TEST
  // Self test of the receive path: no ESP32-S3 and no Bluetooth needed.
  i2s_receiver_config_t test_config = {};
  test_config.bclk_gpio = APP_I2S_BCLK_GPIO;
  test_config.ws_gpio = APP_I2S_WS_GPIO;
  test_config.data_gpio = APP_I2S_DATA_GPIO;
  test_config.sample_rate = settings->input_rate;
  test_config.output_rate = settings->output_rate;
  test_config.shift_bits = APP_I2S_PCM_SHIFT_BITS;
  test_config.gate = NULL;  // accept data even while Bluetooth is idle
  test_config.core_id = APP_I2S_TASK_CORE;
  if (i2s_receiver_start(&test_config)) {
    loopback_test_run(APP_I2S_BCLK_GPIO, APP_I2S_WS_GPIO, APP_I2S_DATA_GPIO,
                      APP_I2S_PCM_SHIFT_BITS, settings->input_rate);
    i2s_receiver_stop();
    audio_buffer_reset();
  }
#endif

  a2dp_source_start();

#if CONFIG_APP_TEST_TONE
  ESP_LOGW(TAG, "TEST TONE mode: the I2S input is ignored");
  if (!tone_source_start(a2dp_source_is_streaming)) {
    ESP_LOGE(TAG, "tone source init failed");
  }
#else
  i2s_receiver_config_t config = {};
  config.bclk_gpio = APP_I2S_BCLK_GPIO;
  config.ws_gpio = APP_I2S_WS_GPIO;
  config.data_gpio = APP_I2S_DATA_GPIO;
  config.sample_rate = settings->input_rate;
  config.output_rate = settings->output_rate;
  config.shift_bits = APP_I2S_PCM_SHIFT_BITS;
  config.gate = a2dp_source_is_streaming;
  config.core_id = APP_I2S_TASK_CORE;
  if (!i2s_receiver_start(&config)) {
    ESP_LOGE(TAG, "I2S receiver init failed - audio will be silent");
  }
#endif

  ESP_LOGI(TAG, "waiting for a speaker (config portal opens after %d s)",
           CONFIG_APP_PORTAL_DELAY_S);
  int64_t next_stats_us =
      esp_timer_get_time() + (int64_t)CONFIG_APP_STATS_INTERVAL_MS * 1000;

  while (true) {
    vTaskDelay(pdMS_TO_TICKS(250));
    service_config_portal();

    if (esp_timer_get_time() >= next_stats_us) {
      log_status();
      next_stats_us = esp_timer_get_time() +
                      (int64_t)CONFIG_APP_STATS_INTERVAL_MS * 1000;
    }
  }
}
