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
#include "audio_buffer.h"
#include "i2s_receiver.h"
#if CONFIG_APP_LOOPBACK_TEST
#include "loopback_test.h"
#endif
#if CONFIG_APP_TEST_TONE
#include "tone_source.h"
#endif

static const char *TAG = "app";

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
  ESP_LOGI(TAG, "BT MAC %02x:%02x:%02x:%02x:%02x:%02x | local name '%s'",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], APP_BT_DEVICE_NAME);
#if CONFIG_SPIRAM
  ESP_LOGI(TAG, "PSRAM %u bytes (audio buffers stay in internal RAM)",
           (unsigned)esp_psram_get_size());
#endif
  ESP_LOGI(TAG, "A2DP %d Hz, I2S input %d Hz", APP_OUTPUT_SAMPLE_RATE,
           APP_INPUT_SAMPLE_RATE);
  if (APP_INPUT_SAMPLE_RATE != APP_OUTPUT_SAMPLE_RATE) {
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

extern "C" void app_main(void) {
  log_environment();

  if (!audio_buffer_init(APP_BUFFER_BYTES)) {
    ESP_LOGE(TAG, "audio buffer init failed - stopping");
    return;
  }
  ESP_LOGI(TAG, "audio buffer: %u frames (%u ms)",
           (unsigned)audio_buffer_get_stats().capacity_frames,
           (unsigned)(audio_buffer_get_stats().capacity_frames * 1000 /
                      APP_OUTPUT_SAMPLE_RATE));

#if CONFIG_APP_LOOPBACK_TEST
  // Self test of the receive path: no ESP32-S3 and no Bluetooth needed.
  i2s_receiver_config_t test_config = {};
  test_config.bclk_gpio = APP_I2S_BCLK_GPIO;
  test_config.ws_gpio = APP_I2S_WS_GPIO;
  test_config.data_gpio = APP_I2S_DATA_GPIO;
  test_config.sample_rate = APP_INPUT_SAMPLE_RATE;
  test_config.shift_bits = APP_I2S_PCM_SHIFT_BITS;
  test_config.gate = NULL;  // accept data even while Bluetooth is idle
  test_config.core_id = APP_I2S_TASK_CORE;
  if (i2s_receiver_start(&test_config)) {
    loopback_test_run(APP_I2S_BCLK_GPIO, APP_I2S_WS_GPIO, APP_I2S_DATA_GPIO,
                      APP_I2S_PCM_SHIFT_BITS, APP_INPUT_SAMPLE_RATE);
    i2s_receiver_stop();
    audio_buffer_reset();
  }
#endif

  a2dp_source_start(APP_BT_TARGET_NAME);

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
  config.sample_rate = APP_INPUT_SAMPLE_RATE;
  config.shift_bits = APP_I2S_PCM_SHIFT_BITS;
  config.gate = a2dp_source_is_streaming;
  config.core_id = APP_I2S_TASK_CORE;
  if (!i2s_receiver_start(&config)) {
    ESP_LOGE(TAG, "I2S receiver init failed - audio will be silent");
  }
#endif

  while (true) {
    vTaskDelay(pdMS_TO_TICKS(CONFIG_APP_STATS_INTERVAL_MS));
    log_status();
  }
}
