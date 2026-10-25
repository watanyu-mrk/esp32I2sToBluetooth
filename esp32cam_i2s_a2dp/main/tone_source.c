#include "tone_source.h"

#include <math.h>
#include <stdlib.h>

#include "audio_buffer.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "tone";
static const double kPi = 3.14159265358979323846;

static volatile bool s_running;
static TaskHandle_t s_task;

static void tone_task(void *arg) {
  audio_gate_t gate = (audio_gate_t)arg;
  const size_t frames = APP_TONE_CHUNK_FRAMES;
  // MALLOC_CAP_8BIT is required: plain internal RAM may include IRAM, which
  // only supports 32-bit accesses, so the int16_t stores below would fault
  // with a LoadStoreError.
  int16_t *chunk = heap_caps_malloc(frames * APP_BYTES_PER_FRAME,
                                    MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (chunk == NULL) {
    ESP_LOGE(TAG, "cannot allocate tone buffer");
    s_task = NULL;
    vTaskDelete(NULL);
    return;
  }

  const double delta = 2.0 * kPi * APP_TONE_FREQ_HZ / APP_OUTPUT_SAMPLE_RATE;
  double phase = 0.0;
  TickType_t last_wake = xTaskGetTickCount();
  bool announced = false;

  while (s_running) {
    if (gate != NULL && !gate()) {
      // Keep the buffer empty while nothing is streaming.
      audio_buffer_reset();
      phase = 0.0;
      announced = false;
      vTaskDelay(pdMS_TO_TICKS(10));
      last_wake = xTaskGetTickCount();
      continue;
    }
    if (!announced) {
      ESP_LOGI(TAG, "feeding %d Hz sine to the A2DP callback",
               APP_TONE_FREQ_HZ);
      announced = true;
    }

    for (size_t i = 0; i < frames; i++) {
      int16_t sample = (int16_t)(APP_TONE_AMPLITUDE * sin(phase));
      chunk[i * 2] = sample;
      chunk[i * 2 + 1] = sample;
      phase += delta;
      if (phase > 2.0 * kPi) phase -= 2.0 * kPi;
    }

    // One chunk is exactly 10 ms, so a 1 kHz tick paces it without drift.
    audio_buffer_write(chunk, frames);
    vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(10));
  }

  free(chunk);
  ESP_LOGI(TAG, "tone task stopped");
  s_task = NULL;
  vTaskDelete(NULL);
}

bool tone_source_start(audio_gate_t gate) {
  if (s_running) return true;
  s_running = true;
  if (xTaskCreatePinnedToCore(tone_task, "tone", 3072, (void *)gate, 5, &s_task,
                              APP_I2S_TASK_CORE) != pdPASS) {
    ESP_LOGE(TAG, "cannot create tone task");
    s_running = false;
    return false;
  }
  return true;
}

void tone_source_stop(void) {
  s_running = false;
  vTaskDelay(pdMS_TO_TICKS(50));
}
