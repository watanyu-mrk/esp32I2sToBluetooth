#include "loopback_test.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "driver/i2s_std.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_config.h"
#include "audio_buffer.h"
#include "resampler.h"

static const char *TAG = "loopback";

#define TEST_FRAMES 128
#define RECEIVE_FRAMES 192
#define SEARCH_FRAMES 4096

static int16_t expected_left(int frame) { return (int16_t)(1000 + frame * 3); }
static int16_t expected_right(int frame) { return (int16_t)(-1000 - frame * 3); }

static void fill_pattern(int32_t *slots, int shift_bits) {
  for (int i = 0; i < TEST_FRAMES; i++) {
    uint32_t left = (uint16_t)expected_left(i);
    uint32_t right = (uint16_t)expected_right(i);
    // The receiver shifts each slot right by shift_bits, so the test places
    // the PCM16 exactly where that receiver expects it.
    slots[i * 2] = (int32_t)(left << shift_bits);
    slots[i * 2 + 1] = (int32_t)(right << shift_bits);
  }
}

// Pure CPU test of the 48 kHz -> 44.1 kHz path: feeds a continuous ramp
// through resampler_process() in chunks and checks the produced frame count,
// monotonicity and that the two channels stay mirrored. A ramp is used
// because linear interpolation reproduces it exactly.
static bool resampler_selftest(void) {
  enum { IN_FRAMES = 256, CHUNKS = 8, OUT_CAPACITY = 300 };
  static int16_t in[IN_FRAMES * APP_AUDIO_CHANNELS];
  static int16_t out[OUT_CAPACITY * APP_AUDIO_CHANNELS];
  resampler_t resampler;
  resampler_init(&resampler, 48000, 44100);

  size_t produced = 0;
  int16_t previous = -1;
  int step_min = 1000;
  int step_max = 0;

  for (int chunk = 0; chunk < CHUNKS; chunk++) {
    for (int i = 0; i < IN_FRAMES; i++) {
      int16_t value = (int16_t)((chunk * IN_FRAMES + i) * 10);
      in[i * APP_AUDIO_CHANNELS] = value;
      in[i * APP_AUDIO_CHANNELS + 1] = (int16_t)-value;
    }

    size_t count = resampler_process(&resampler, in, IN_FRAMES, out,
                                     OUT_CAPACITY);
    for (size_t k = 0; k < count; k++) {
      int16_t left = out[k * APP_AUDIO_CHANNELS];
      int16_t right = out[k * APP_AUDIO_CHANNELS + 1];
      if (right != (int16_t)-left) {
        ESP_LOGE(TAG, "RESAMPLER FAIL: channels not mirrored (%d / %d)", left,
                 right);
        return false;
      }
      if (previous >= 0) {
        int step = left - previous;
        if (step < step_min) step_min = step;
        if (step > step_max) step_max = step;
        if (step <= 0) {
          ESP_LOGE(TAG, "RESAMPLER FAIL: ramp is not increasing (%d -> %d)",
                   previous, left);
          return false;
        }
      }
      previous = left;
    }
    produced += count;
  }

  // 2048 input frames at 44.1/48 kHz must yield ~1881.6 output frames, and
  // the 10-per-input-frame ramp must advance by 10 * 48000/44100 = ~10.88
  // per output frame.
  double expected = (double)(CHUNKS * IN_FRAMES) * 44100.0 / 48000.0;
  if (produced + 2 < expected || produced > expected + 2) {
    ESP_LOGE(TAG, "RESAMPLER FAIL: produced %u frames, expected %.1f",
             (unsigned)produced, expected);
    return false;
  }
  if (step_min < 10 || step_max > 12) {
    ESP_LOGE(TAG, "RESAMPLER FAIL: ramp step %d..%d, expected ~11", step_min,
             step_max);
    return false;
  }

  ESP_LOGI(TAG, "RESAMPLER PASS: %u/%.1f frames, step %d..%d",
           (unsigned)produced, expected, step_min, step_max);
  return true;
}

bool loopback_test_run(int bclk_gpio, int ws_gpio, int data_gpio,
                       int shift_bits, uint32_t sample_rate) {
  bool resampler_ok = resampler_selftest();

  ESP_LOGI(TAG, "starting I2S loopback test (TX on BCLK=%d WS=%d DATA=%d, "
                "shift=%d)",
           bclk_gpio, ws_gpio, data_gpio, shift_bits);

  i2s_chan_config_t tx_channel =
      I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
  tx_channel.dma_desc_num = 4;
  tx_channel.dma_frame_num = TEST_FRAMES;
  tx_channel.auto_clear = true;

  i2s_chan_handle_t tx = NULL;
  esp_err_t err = i2s_new_channel(&tx_channel, &tx, NULL);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "I2S1 TX channel allocation failed: %s",
             esp_err_to_name(err));
    return false;
  }

  i2s_std_config_t tx_config = {
      .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate),
      .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT,
                                                     I2S_SLOT_MODE_STEREO),
      .gpio_cfg = {0},
  };
  tx_config.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_32BIT;
  tx_config.gpio_cfg.mclk = I2S_GPIO_UNUSED;
  tx_config.gpio_cfg.bclk = (gpio_num_t)bclk_gpio;
  tx_config.gpio_cfg.ws = (gpio_num_t)ws_gpio;
  tx_config.gpio_cfg.dout = (gpio_num_t)data_gpio;
  tx_config.gpio_cfg.din = I2S_GPIO_UNUSED;

  err = i2s_channel_init_std_mode(tx, &tx_config);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "I2S1 TX init failed: %s", esp_err_to_name(err));
    i2s_del_channel(tx);
    return false;
  }

  int32_t *slots = heap_caps_malloc(TEST_FRAMES * APP_BYTES_PER_FRAME * 2,
                                    MALLOC_CAP_INTERNAL);
  if (slots == NULL) {
    ESP_LOGE(TAG, "cannot allocate loopback pattern");
    i2s_del_channel(tx);
    return false;
  }
  fill_pattern(slots, shift_bits);

  // Drop anything the receiver queued before the transmitter existed.
  audio_buffer_reset();

  err = i2s_channel_enable(tx);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "I2S1 TX enable failed: %s", esp_err_to_name(err));
    free(slots);
    i2s_del_channel(tx);
    return false;
  }

  const bool resampled = sample_rate != APP_OUTPUT_SAMPLE_RATE;
  const double step = (double)sample_rate / (double)APP_OUTPUT_SAMPLE_RATE;
  // Pattern frames that fit in the pattern when it is resampled (the last
  // input frame is only reachable through interpolation of the silence
  // that follows it).
  size_t pattern_frames = TEST_FRAMES;
  if (resampled) {
    pattern_frames = (size_t)floor((double)(TEST_FRAMES - 1) / step) + 1;
  }
  // Collect a little more than the pattern so the end of the ramp is visible.
  size_t wanted = resampled ? pattern_frames + 8 : TEST_FRAMES;
  if (wanted > RECEIVE_FRAMES) wanted = RECEIVE_FRAMES;

  size_t written = 0;
  err = i2s_channel_write(tx, slots, (size_t)TEST_FRAMES * APP_BYTES_PER_FRAME * 2,
                          &written, pdMS_TO_TICKS(1000));
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "loopback write failed: %s", esp_err_to_name(err));
  }
  vTaskDelay(pdMS_TO_TICKS(250));

  i2s_channel_disable(tx);
  i2s_del_channel(tx);
  free(slots);

  // Look for the pattern in what the receiver buffered. With matching rates
  // the first frame is exact; when the receiver resamples, the interpolation
  // phase depends on where its first I2S read landed, so only the shape of
  // the ramp (mirrored channels, density) can be checked.
  int16_t chunk[128 * APP_AUDIO_CHANNELS];
  size_t scanned = 0;
  int16_t received[RECEIVE_FRAMES * APP_AUDIO_CHANNELS];
  size_t received_frames = 0;

  while (scanned < SEARCH_FRAMES && received_frames < wanted) {
    size_t got = audio_buffer_read(chunk, 128);
    if (got == 0) break;
    scanned += got;

    for (size_t i = 0; i < got && received_frames < wanted; i++) {
      if (received_frames == 0) {
        bool start = resampled
                         ? (chunk[i * 2] != 0 || chunk[i * 2 + 1] != 0)
                         : (chunk[i * 2] == expected_left(0) &&
                            chunk[i * 2 + 1] == expected_right(0));
        if (!start) continue;
      }
      received[received_frames * 2 + 0] = chunk[i * 2];
      received[received_frames * 2 + 1] = chunk[i * 2 + 1];
      received_frames++;
    }
  }

  if (received_frames < wanted) {
    ESP_LOGE(TAG,
             "LOOPBACK FAIL: found %u/%u frames after scanning %u frames "
             "(no signal, wrong pins, or wrong shift)",
             (unsigned)received_frames, (unsigned)wanted, (unsigned)scanned);
    return false;
  }

  if (resampled) {
    // Channels must stay mirrored, and the ramp must survive the conversion:
    // 3 * step (~3.27) per output frame over about TEST_FRAMES / step frames.
    // A missing or wrong resampler breaks the density, a swapped channel pair
    // breaks the mirror.
    size_t best_run = 0;
    size_t run = 0;
    for (size_t i = 0; i < received_frames; i++) {
      int16_t left = received[i * 2];
      int16_t right = received[i * 2 + 1];
      if (right != (int16_t)-left) {
        ESP_LOGE(TAG,
                 "LOOPBACK FAIL (resampled): L/R order broken at frame %u "
                 "(L=%d R=%d)",
                 (unsigned)i, left, right);
        return false;
      }
      if (i == 0) continue;

      int delta = (int)left - (int)received[(i - 1) * 2];
      if (delta >= 3 && delta <= 4) {
        run++;
        if (run > best_run) best_run = run;
      } else {
        run = 0;
      }
    }

    // best_run counts steps, so the ramp spans best_run + 1 frames.
    size_t ramp_frames = best_run + 1;
    if (ramp_frames + 6 < pattern_frames || ramp_frames > pattern_frames + 6) {
      ESP_LOGE(TAG,
               "LOOPBACK FAIL (resampled): ramp is %u frames, expected ~%u "
               "(resampler not applied, or wrong ratio)",
               (unsigned)ramp_frames, (unsigned)pattern_frames);
      return false;
    }

    ESP_LOGI(TAG,
             "LOOPBACK PASS (resampled %u -> %d Hz): %u frame ramp, ~%.2f "
             "per output frame (shift=%d, L/R order correct)",
             (unsigned)sample_rate, APP_OUTPUT_SAMPLE_RATE,
             (unsigned)ramp_frames, 3.0 * step, shift_bits);
    return resampler_ok;
  }

  for (size_t i = 0; i < TEST_FRAMES; i++) {
    int16_t left = received[i * 2];
    int16_t right = received[i * 2 + 1];
    if (left != expected_left((int)i) || right != expected_right((int)i)) {
      ESP_LOGE(TAG,
               "LOOPBACK FAIL: frame %u expected L=%d R=%d, got L=%d R=%d",
               (unsigned)i, expected_left((int)i), expected_right((int)i),
               left, right);
      return false;
    }
  }

  ESP_LOGI(TAG,
           "LOOPBACK PASS: %d frames verified (32-bit slots, shift=%d, "
           "L/R order correct)",
           TEST_FRAMES, shift_bits);
  return resampler_ok;
}
