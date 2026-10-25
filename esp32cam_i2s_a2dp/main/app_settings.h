// Runtime configuration, editable from the AP config page and stored in NVS.
//
// The Kconfig values are only the factory defaults: everything listed here is
// read once at boot, so changing a setting from the browser does not need a
// rebuild - just a reboot.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Bluetooth names are limited to 248 bytes, but keeping them short keeps the
// page and the discovery matching simple.
#define APP_SETTINGS_NAME_LEN 33

typedef struct {
  char target_name[APP_SETTINGS_NAME_LEN];  // empty: connect to the first sink
  char local_name[APP_SETTINGS_NAME_LEN];
  bool ssp_enabled;
  uint32_t input_rate;   // I2S rate of the ESP32-S3 (descriptive: slave)
  uint32_t output_rate;  // PCM rate fed to A2DP
  uint32_t buffer_ms;
} app_settings_t;

// Loads the stored settings, falling back to the Kconfig defaults for missing
// or invalid values. Call once, before anything reads app_settings_get().
void app_settings_load(void);

// Current settings. Never NULL; treat as read-only.
const app_settings_t *app_settings_get(void);

// Validates and persists a candidate. On success the caller is expected to
// restart so that every consumer picks up the new values.
bool app_settings_save(const app_settings_t *candidate);

// Copies the Kconfig defaults into out (used for the initial NVS contents and
// for a factory reset from the config page).
void app_settings_defaults(app_settings_t *out);

#ifdef __cplusplus
}
#endif
