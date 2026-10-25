#include "bt_scan.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"

#include "app_settings.h"

static const char *TAG = "bt_scan";

static bt_scan_device_t s_devices[BT_SCAN_MAX_DEVICES];
static size_t s_count;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

void bt_scan_clear(void) {
  portENTER_CRITICAL(&s_mux);
  memset(s_devices, 0, sizeof(s_devices));
  s_count = 0;
  portEXIT_CRITICAL(&s_mux);
}

void bt_scan_record(const char *name, const uint8_t address[6], int rssi) {
  if (address == NULL) return;

  char text[APP_SETTINGS_NAME_LEN];
  snprintf(text, sizeof(text), "%s",
           (name != NULL && name[0] != '\0') ? name : "(ไม่มีชื่อ)");

  char formatted[18];
  snprintf(formatted, sizeof(formatted), "%02x:%02x:%02x:%02x:%02x:%02x",
           address[0], address[1], address[2], address[3], address[4],
           address[5]);

  portENTER_CRITICAL(&s_mux);

  // Same device seen again: refresh the name and signal instead of adding a
  // duplicate entry.
  for (size_t i = 0; i < s_count; i++) {
    if (memcmp(s_devices[i].address, formatted, sizeof(formatted)) == 0) {
      snprintf(s_devices[i].name, sizeof(s_devices[i].name), "%s", text);
      s_devices[i].rssi = rssi;
      portEXIT_CRITICAL(&s_mux);
      return;
    }
  }

  bt_scan_device_t *slot = NULL;
  if (s_count < BT_SCAN_MAX_DEVICES) {
    slot = &s_devices[s_count++];
  } else {
    // Full: evict the weakest device so a new one can still be picked.
    int weakest = 0;
    for (size_t i = 1; i < BT_SCAN_MAX_DEVICES; i++) {
      if (s_devices[i].rssi < s_devices[weakest].rssi) weakest = (int)i;
    }
    if (rssi <= s_devices[weakest].rssi) {
      portEXIT_CRITICAL(&s_mux);
      return;
    }
    slot = &s_devices[weakest];
  }

  memset(slot, 0, sizeof(*slot));
  snprintf(slot->name, sizeof(slot->name), "%s", text);
  snprintf(slot->address, sizeof(slot->address), "%s", formatted);
  slot->rssi = rssi;
  slot->used = true;

  portEXIT_CRITICAL(&s_mux);
  ESP_LOGI(TAG, "sink %s (%s, %d dBm)", text, formatted, rssi);
}

size_t bt_scan_list(bt_scan_device_t *out, size_t max) {
  if (out == NULL || max == 0) return 0;

  portENTER_CRITICAL(&s_mux);
  size_t count = s_count < max ? s_count : max;
  memcpy(out, s_devices, count * sizeof(bt_scan_device_t));
  portEXIT_CRITICAL(&s_mux);

  // Strongest first: the speaker in the same room should be at the top.
  for (size_t i = 1; i < count; i++) {
    bt_scan_device_t key = out[i];
    size_t j = i;
    while (j > 0 && out[j - 1].rssi < key.rssi) {
      out[j] = out[j - 1];
      j--;
    }
    out[j] = key;
  }
  return count;
}
