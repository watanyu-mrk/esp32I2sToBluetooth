#include "pin_activity.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "pin_check";

// Reads per timer check: keeps the sampling loop tight (well above the audio
// rates we care about) without calling esp_timer_get_time() every sample.
#define SAMPLES_PER_TICK_CHECK 128

bool pin_activity_measure(const int *gpios, size_t count, uint32_t window_ms,
                          pin_activity_t *activity) {
  if (gpios == NULL || activity == NULL) return false;
  if (count == 0 || count > PIN_ACTIVITY_MAX) return false;

  for (size_t i = 0; i < count; i++) {
    if (gpios[i] < 0) {
      ESP_LOGE(TAG, "invalid GPIO %d", gpios[i]);
      return false;
    }

    gpio_config_t config = {
        .pin_bit_mask = 1ULL << gpios[i],
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&config);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "gpio_config(%d) failed: %s", gpios[i],
               esp_err_to_name(err));
      return false;
    }

    activity[i].gpio = gpios[i];
    activity[i].transitions = 0;
    activity[i].window_ms = window_ms;
    activity[i].last_level = gpio_get_level((gpio_num_t)gpios[i]);
  }

  int previous[PIN_ACTIVITY_MAX];
  for (size_t i = 0; i < count; i++) previous[i] = activity[i].last_level;

  const int64_t deadline = esp_timer_get_time() + (int64_t)window_ms * 1000;
  while (esp_timer_get_time() < deadline) {
    for (int n = 0; n < SAMPLES_PER_TICK_CHECK; n++) {
      for (size_t i = 0; i < count; i++) {
        int level = gpio_get_level((gpio_num_t)gpios[i]);
        if (level != previous[i]) {
          previous[i] = level;
          activity[i].transitions++;
        }
        activity[i].last_level = level;
      }
    }
  }

  return true;
}

bool pin_activity_is_toggling(const pin_activity_t *activity) {
  if (activity == NULL) return false;
  return activity->transitions >= PIN_ACTIVITY_ACTIVE_EDGES;
}
