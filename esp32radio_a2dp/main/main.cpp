// ESP32-WROVER Internet radio -> Bluetooth speaker:
//
//   MP3 stream (HTTP/HTTPS) --decode--> PCM16 stereo 44100 Hz
//        --jitter buffer--> Bluetooth A2DP source -> speaker
//
// The board is a client on the local network and also serves the config page
// (over that network, or over its own setup access point when it cannot join),
// so the station URL and the Bluetooth pairing can be changed without a
// rebuild.

#include <inttypes.h>
#include <string.h>

#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_mac.h"
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
#include "radio_stream.h"
#include "wifi_link.h"

static const char *TAG = "app";

static void log_environment(void) {
  esp_chip_info_t chip_info;
  esp_chip_info(&chip_info);

  uint32_t flash_size = 0;
  esp_flash_get_size(NULL, &flash_size);

  uint8_t mac[6] = {0};
  esp_read_mac(mac, ESP_MAC_BT);

  const app_settings_t *settings = app_settings_get();

  ESP_LOGI(TAG, "Internet radio -> Bluetooth A2DP source");
  ESP_LOGI(TAG, "ESP-IDF %s | %d core(s) | flash %" PRIu32 " MB",
           esp_get_idf_version(), chip_info.cores,
           flash_size / (1024 * 1024));
  ESP_LOGI(TAG, "BT MAC %02x:%02x:%02x:%02x:%02x:%02x | local name '%s'",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
           settings->local_name);
  ESP_LOGI(TAG, "%u station(s), first '%s': %s",
           (unsigned)settings->station_count,
           settings->stations[0].name[0] ? settings->stations[0].name
                                         : "(unnamed)",
           settings->stations[0].url);
  ESP_LOGI(TAG, "Wi-Fi '%s' | A2DP %d Hz | startup volume %u/127",
           settings->wifi_ssid[0] ? settings->wifi_ssid : "(not configured)",
           APP_OUTPUT_SAMPLE_RATE, (unsigned)settings->volume);
#if CONFIG_SPIRAM
  ESP_LOGI(TAG, "PSRAM %u bytes (jitter buffer and decode scratch live there)",
           (unsigned)esp_psram_get_size());
#endif
}

static void log_status(void) {
  radio_stream_info_t radio;
  radio_stream_get_info(&radio);
  audio_buffer_stats_t buffer = audio_buffer_get_stats();

  unsigned percent =
      buffer.capacity_frames > 0
          ? (unsigned)(buffer.available_frames * 100 / buffer.capacity_frames)
          : 0;

  ESP_LOGI(TAG,
           "Wi-Fi %s %s (%d dBm) | BT %s | radio %s %" PRIu32 " kbps "
           "%" PRIu32 " Hz %uch | buf %3u%% (%u/%u fr) | drop %" PRIu32
           ", underflow %" PRIu32 ", decode err %" PRIu32 ", retry %" PRIu32
           " | heap %u",
           wifi_link_state_text(), wifi_link_ip(), wifi_link_rssi(),
           a2dp_source_state(), radio.state, radio.bitrate, radio.sample_rate,
           (unsigned)radio.channels, percent,
           (unsigned)buffer.available_frames, (unsigned)buffer.capacity_frames,
           buffer.dropped_frames, buffer.underflow_frames,
           radio.decode_errors, radio.failures,
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
}

extern "C" void app_main(void) {
  app_settings_load();
  log_environment();

  const app_settings_t *settings = app_settings_get();
  size_t buffer_bytes =
      (size_t)APP_OUTPUT_SAMPLE_RATE * APP_BYTES_PER_FRAME *
      settings->buffer_ms / 1000;
  if (!audio_buffer_init(buffer_bytes)) {
    ESP_LOGE(TAG, "audio buffer init failed - stopping");
    return;
  }
  ESP_LOGI(TAG, "audio buffer: %u frames (%u ms)",
           (unsigned)audio_buffer_get_stats().capacity_frames,
           (unsigned)(audio_buffer_get_stats().capacity_frames * 1000 /
                      APP_OUTPUT_SAMPLE_RATE));

  if (!wifi_link_start()) {
    ESP_LOGE(TAG, "Wi-Fi init failed - no station and no config page");
  }
  if (!config_portal_start()) {
    ESP_LOGE(TAG, "config page failed to start");
  }

  a2dp_source_start();
  radio_stream_start();

  int64_t next_stats_us =
      esp_timer_get_time() + (int64_t)CONFIG_APP_STATS_INTERVAL_MS * 1000;

  while (true) {
    vTaskDelay(pdMS_TO_TICKS(200));
    wifi_link_poll();

    // Start pulling the station as soon as the speaker is connected, not only
    // once it starts playing: that way the jitter buffer is already full when
    // the first PCM is asked for. Filling late leaves the Bluetooth stack with
    // nothing to send, so it pads silence, keeps the shared radio busy, and
    // starves the very Wi-Fi reads that would refill the buffer (audible as
    // constant stutter). Once the buffer is full the writer simply waits.
    radio_stream_set_enabled(a2dp_source_is_connected());

    if (esp_timer_get_time() >= next_stats_us) {
      log_status();
      next_stats_us = esp_timer_get_time() +
                      (int64_t)CONFIG_APP_STATS_INTERVAL_MS * 1000;
    }
  }
}
