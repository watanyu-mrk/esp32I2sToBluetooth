// Cache of the A2DP sinks that Bluetooth discovery has reported, so the config
// page can list them without running its own inquiry (the A2DP source is
// already scanning while it looks for its target).
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "app_settings.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BT_SCAN_MAX_DEVICES 8

typedef struct {
  char name[APP_SETTINGS_NAME_LEN];
  char address[18];  // "aa:bb:cc:dd:ee:ff"
  int rssi;
  bool used;
} bt_scan_device_t;

// Forgets every recorded device (used by the "search again" button).
void bt_scan_clear(void);

// Adds a device or refreshes its RSSI if the address is already known.
void bt_scan_record(const char *name, const uint8_t address[6], int rssi);

// Copies up to max devices, strongest signal first. Returns the count.
size_t bt_scan_list(bt_scan_device_t *out, size_t max);

#ifdef __cplusplus
}
#endif
