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

// The config portal (Wi-Fi AP + web page) only runs while no speaker is
// connected: Wi-Fi and Bluetooth Classic share the radio and the heap, and the
// A2DP stream needs both. A short delay after boot leaves the reconnect path
// a chance to win before the portal costs anything.
#define PORTAL_FIRST_DELAY_MS 3000
#define PORTAL_REOPEN_DELAY_MS 30000
#define PORTAL_RETRY_MS 10000
// The Wi-Fi AP plus the HTTP server need roughly 30 KB; below this the
// Bluetooth stack would start failing its own allocations.
#define PORTAL_MIN_HEAP_BYTES (32 * 1024)

static int64_t s_disconnected_since_us;
static size_t s_buffer_bytes;

// Nothing is streaming while the portal is up (the audio gate is closed), so
// the ring buffer is dead weight. Its ~64 KB is exactly what the Wi-Fi stack
// and the HTTP server need: handing it over is what makes the portal fit.
static void release_audio_buffer(void) {
  audio_buffer_deinit();
  ESP_LOGI(TAG, "audio buffer released for the config portal (%u bytes free)",
           (unsigned)esp_get_free_heap_size());
}

static void restore_audio_buffer(void) {
  if (!audio_buffer_init(s_buffer_bytes)) {
    ESP_LOGE(TAG, "cannot reallocate the audio buffer after the config portal");
  }
}

static void service_config_portal(bool *opened_once) {
  // Close as soon as a connection starts, not when it completes: the stack
  // allocates its streaming buffers (several 4 KB packets) right after that,
  // and the Wi-Fi stack is still holding ~30 KB at this point.
  if (a2dp_source_is_active()) {
    s_disconnected_since_us = 0;
    if (config_portal_is_running()) {
      ESP_LOGI(TAG, "speaker link starting: closing the config portal");
      config_portal_stop();
      restore_audio_buffer();
    }
    return;
  }

  if (config_portal_is_running()) return;

  int64_t now = esp_timer_get_time();
  if (s_disconnected_since_us == 0) s_disconnected_since_us = now;

  int64_t delay_ms = *opened_once ? PORTAL_REOPEN_DELAY_MS : PORTAL_FIRST_DELAY_MS;
  if (now - s_disconnected_since_us < delay_ms * 1000) return;

  // Retry slowly: the portal costs a lot of heap and the Bluetooth stack is
  // still allocating its buffers, so a failing attempt should not be repeated
  // every tick.
  static int64_t last_attempt_us;
  if (last_attempt_us != 0 && now - last_attempt_us < PORTAL_RETRY_MS * 1000) {
    return;
  }
  last_attempt_us = now;

  release_audio_buffer();

  // Still leave headroom for the Bluetooth stack: it has to survive the portal
  // and allocate its streaming buffers as soon as a speaker answers.
  if (esp_get_free_heap_size() < PORTAL_MIN_HEAP_BYTES) {
    ESP_LOGW(TAG, "not enough free heap for the config portal (%u bytes)",
             (unsigned)esp_get_free_heap_size());
    restore_audio_buffer();
    return;
  }

  if (config_portal_start()) {
    *opened_once = true;
  } else {
    restore_audio_buffer();
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
  s_buffer_bytes = (size_t)settings->output_rate * APP_BYTES_PER_FRAME *
                   settings->buffer_ms / 1000;
  if (!audio_buffer_init(s_buffer_bytes)) {
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

  bool portal_opened = false;
  int64_t next_stats_us =
      esp_timer_get_time() + (int64_t)CONFIG_APP_STATS_INTERVAL_MS * 1000;

  while (true) {
    vTaskDelay(pdMS_TO_TICKS(250));
    service_config_portal(&portal_opened);

    if (esp_timer_get_time() >= next_stats_us) {
      log_status();
      next_stats_us = esp_timer_get_time() +
                      (int64_t)CONFIG_APP_STATS_INTERVAL_MS * 1000;
    }
  }
}
