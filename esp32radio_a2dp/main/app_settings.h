// Runtime configuration, editable from the web page and stored in NVS.
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

// esp_bt_pin_code_t holds at most 16 digits, plus the terminator.
#define APP_SETTINGS_PIN_LEN 17

// A stream URL has to fit the page field and the HTTP client's config.
#define APP_SETTINGS_URL_LEN 256

// 802.11 SSIDs are at most 32 bytes; WPA2 passphrases 8-63 characters.
#define APP_SETTINGS_SSID_LEN 33
#define APP_SETTINGS_PASSWORD_LEN 65

#define APP_SETTINGS_BUFFER_MIN_MS 200
#define APP_SETTINGS_BUFFER_MAX_MS 3000

// A2DP volume range: 127 is the speaker's maximum, 0 is silent.
#define APP_SETTINGS_VOLUME_MAX 127

// How many stations the remote's next/previous buttons can cycle through. Each
// station costs 256 + 33 bytes, so the full list is ~6 KB: the buffers that
// only exist while settings are loaded or saved live in PSRAM for that reason.
#define APP_SETTINGS_MAX_STATIONS 20

typedef struct {
  char url[APP_SETTINGS_URL_LEN];
  char name[APP_SETTINGS_NAME_LEN];  // empty: the page shows the URL
} app_station_t;

typedef struct {
  app_station_t stations[APP_SETTINGS_MAX_STATIONS];
  uint8_t station_count;  // 1..APP_SETTINGS_MAX_STATIONS
  char wifi_ssid[APP_SETTINGS_SSID_LEN];
  char wifi_password[APP_SETTINGS_PASSWORD_LEN];
  char target_name[APP_SETTINGS_NAME_LEN];  // empty: connect to the first sink
  char local_name[APP_SETTINGS_NAME_LEN];
  // Legacy pairing PIN (1-16 digits), e.g. "0000" for many car head units.
  // Empty: leave the library default ("1234") in place.
  char pin_code[APP_SETTINGS_PIN_LEN];
  bool ssp_enabled;
  uint32_t buffer_ms;
  // Level pushed to the speaker whenever it starts playing (0-127). Without
  // it the speaker keeps its own volume, i.e. full blast after every boot.
  uint8_t volume;
} app_settings_t;

// Loads the stored settings, falling back to the Kconfig defaults for missing
// or invalid values. Call once, before anything reads app_settings_get().
// Settings written by the single-station layout are migrated, not dropped.
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
