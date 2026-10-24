#include "i2s_receiver.h"

#include <string.h>

#include "driver/i2s_std.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "audio_buffer.h"
#include "resampler.h"

static const char *TAG = "i2s_rx";

static i2s_chan_handle_t s_rx_handle;
static TaskHandle_t s_task_handle;
static volatile bool s_running;
static i2s_receiver_config_t s_config;
static resampler_t s_resampler;

// Worst case for the resampler is upsampling 44.1 kHz -> 48 kHz (~1.09x).
#define APP_RESAMPLED_MAX_FRAMES (APP_I2S_CHUNK_FRAMES * 2 + 16)

static int32_t s_raw_slots[APP_I2S_CHUNK_FRAMES * APP_AUDIO_CHANNELS];
static int16_t s_pcm[APP_I2S_CHUNK_FRAMES * APP_AUDIO_CHANNELS];
static int16_t
    s_resampled[APP_RESAMPLED_MAX_FRAMES * APP_AUDIO_CHANNELS];

static i2s_receiver_stats_t s_stats;
static portMUX_TYPE s_stats_mux = portMUX_INITIALIZER_UNLOCKED;

static void i2s_receive_task(void *arg) {
  (void)arg;
  ESP_LOGI(TAG, "receiver task running on core %d", xPortGetCoreID());

  const size_t slot_bytes = sizeof(int32_t) * APP_AUDIO_CHANNELS;
  bool gate_was_closed = false;

  while (s_running) {
    size_t bytes_read = 0;
    esp_err_t err = i2s_channel_read(s_rx_handle, s_raw_slots,
                                     sizeof(s_raw_slots), &bytes_read,
                                     pdMS_TO_TICKS(APP_I2S_READ_TIMEOUT_MS));

    if (err == ESP_ERR_TIMEOUT) {
      // Normal while the ESP32-S3 is not sending: no BCLK, no DMA progress.
      portENTER_CRITICAL(&s_stats_mux);
      s_stats.timeouts++;
      portEXIT_CRITICAL(&s_stats_mux);
      continue;
    }
    if (err != ESP_OK) {
      ESP_LOGW(TAG, "i2s_channel_read failed: %s", esp_err_to_name(err));
      portENTER_CRITICAL(&s_stats_mux);
      s_stats.read_errors++;
      portEXIT_CRITICAL(&s_stats_mux);
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }
    if (bytes_read == 0) continue;

    size_t frames = bytes_read / slot_bytes;
    if (frames == 0) continue;

    portENTER_CRITICAL(&s_stats_mux);
    s_stats.bytes_read += bytes_read;
    portEXIT_CRITICAL(&s_stats_mux);

    // 16-bit PCM sits left aligned in the 32-bit slot (upper half).
    for (size_t i = 0; i < frames * APP_AUDIO_CHANNELS; i++) {
      s_pcm[i] = (int16_t)(s_raw_slots[i] >> s_config.shift_bits);
    }

    if (s_config.gate != NULL && !s_config.gate()) {
      // Nothing is streaming: drop the input instead of queueing stale audio
      // that would be played when the speaker connects.
      portENTER_CRITICAL(&s_stats_mux);
      s_stats.gate_dropped_frames += (uint32_t)frames;
      portEXIT_CRITICAL(&s_stats_mux);
      audio_buffer_reset();
      gate_was_closed = true;
      continue;
    }
    if (gate_was_closed) {
      ESP_LOGI(TAG, "streaming started, buffer reset");
      audio_buffer_reset();
      gate_was_closed = false;
    }

    if (APP_INPUT_SAMPLE_RATE != APP_OUTPUT_SAMPLE_RATE) {
      size_t converted =
          resampler_process(&s_resampler, s_pcm, frames, s_resampled,
                            APP_RESAMPLED_MAX_FRAMES);
      portENTER_CRITICAL(&s_stats_mux);
      s_stats.resampled_frames += (uint32_t)converted;
      portEXIT_CRITICAL(&s_stats_mux);
      audio_buffer_write(s_resampled, converted);
    } else {
      audio_buffer_write(s_pcm, frames);
    }
  }

  ESP_LOGI(TAG, "receiver task stopped");
  s_task_handle = NULL;
  vTaskDelete(NULL);
}

bool i2s_receiver_start(const i2s_receiver_config_t *config) {
  if (s_running) return true;
  if (config == NULL) return false;
  s_config = *config;

  i2s_chan_config_t channel_config =
      I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_SLAVE);
  channel_config.dma_desc_num = 8;
  channel_config.dma_frame_num = APP_I2S_CHUNK_FRAMES;
  channel_config.auto_clear = true;

  esp_err_t err = i2s_new_channel(&channel_config, NULL, &s_rx_handle);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "i2s_new_channel failed: %s", esp_err_to_name(err));
    return false;
  }

  i2s_std_config_t std_config = {
      .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(config->sample_rate),
      .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT,
                                                     I2S_SLOT_MODE_STEREO),
      .gpio_cfg = {0},
  };
  // 32-bit slots carrying 16-bit PCM in the upper half.
  std_config.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_32BIT;
  std_config.gpio_cfg.mclk = I2S_GPIO_UNUSED;
  std_config.gpio_cfg.bclk = (gpio_num_t)config->bclk_gpio;
  std_config.gpio_cfg.ws = (gpio_num_t)config->ws_gpio;
  std_config.gpio_cfg.dout = I2S_GPIO_UNUSED;
  std_config.gpio_cfg.din = (gpio_num_t)config->data_gpio;

  err = i2s_channel_init_std_mode(s_rx_handle, &std_config);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "i2s_channel_init_std_mode failed: %s",
             esp_err_to_name(err));
    i2s_del_channel(s_rx_handle);
    s_rx_handle = NULL;
    return false;
  }

  err = i2s_channel_enable(s_rx_handle);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "i2s_channel_enable failed: %s", esp_err_to_name(err));
    i2s_del_channel(s_rx_handle);
    s_rx_handle = NULL;
    return false;
  }

  memset(&s_stats, 0, sizeof(s_stats));
  resampler_init(&s_resampler, APP_INPUT_SAMPLE_RATE, APP_OUTPUT_SAMPLE_RATE);
  s_running = true;

  BaseType_t created =
      xTaskCreatePinnedToCore(i2s_receive_task, "i2s_rx", APP_I2S_TASK_STACK,
                              NULL, APP_I2S_TASK_PRIO, &s_task_handle,
                              config->core_id);
  if (created != pdPASS) {
    ESP_LOGE(TAG, "cannot create receiver task");
    s_running = false;
    i2s_channel_disable(s_rx_handle);
    i2s_del_channel(s_rx_handle);
    s_rx_handle = NULL;
    return false;
  }

  ESP_LOGI(TAG, "slave RX: BCLK=%d WS=%d DIN=%d, 32-bit slots, shift=%d",
           config->bclk_gpio, config->ws_gpio, config->data_gpio,
           config->shift_bits);
  return true;
}

void i2s_receiver_stop(void) {
  if (!s_running) return;
  s_running = false;
  // Wait for the task to leave its blocking read before deleting the channel.
  for (int i = 0; i < 100 && s_task_handle != NULL; i++) {
    vTaskDelay(pdMS_TO_TICKS(10));
  }

  if (s_rx_handle != NULL) {
    i2s_channel_disable(s_rx_handle);
    i2s_del_channel(s_rx_handle);
    s_rx_handle = NULL;
  }
}

i2s_receiver_stats_t i2s_receiver_get_stats(void) {
  i2s_receiver_stats_t copy;
  portENTER_CRITICAL(&s_stats_mux);
  copy = s_stats;
  portEXIT_CRITICAL(&s_stats_mux);
  return copy;
}
