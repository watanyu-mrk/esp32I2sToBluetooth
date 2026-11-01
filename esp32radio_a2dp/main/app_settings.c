#include "app_settings.h"

#include <inttypes.h>
#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "app_config.h"

static const char *TAG = "settings";

#define NVS_NAMESPACE "appcfg"
#define NVS_KEY "cfg"
// Bumped when app_settings_t changes shape. Version 1 was the single-station
// layout, which is migrated on load instead of being thrown away.
#define SETTINGS_VERSION 2

typedef struct {
  uint8_t version;
  app_settings_t settings;
} stored_blob_t;

// Version 1 stored exactly one station.
typedef struct {
  uint8_t version;
  struct {
    char station_url[APP_SETTINGS_URL_LEN];
    char station_name[APP_SETTINGS_NAME_LEN];
    char wifi_ssid[APP_SETTINGS_SSID_LEN];
    char wifi_password[APP_SETTINGS_PASSWORD_LEN];
    char target_name[APP_SETTINGS_NAME_LEN];
    char local_name[APP_SETTINGS_NAME_LEN];
    char pin_code[APP_SETTINGS_PIN_LEN];
    bool ssp_enabled;
    uint32_t buffer_ms;
  } settings;
} stored_blob_v1_t;

static app_settings_t s_settings;
static bool s_loaded;

// The stored blob is ~2 KB. Keeping these off the stack is what lets the
// settings code run on the 6 KB main task: the stack copies overflowed it
// during hardware testing (the board rebooted before app_main logged).
static stored_blob_t s_blob;
static stored_blob_v1_t s_old_blob;
static app_settings_t s_candidate;

// Guarantees a terminated string even if the caller filled the whole array.
static void force_terminator(char *text, size_t size) {
  text[size - 1] = '\0';
}

// Empty means "not configured" (the A2DP library keeps its own default);
// anything else must be 1-16 digits, because that is what the Bluetooth
// stack's esp_bt_pin_code_t and the remote's keypad can handle.
static bool is_valid_pin(const char *pin) {
  if (pin[0] == '\0') return true;

  size_t len = strlen(pin);
  if (len > APP_SETTINGS_PIN_LEN - 1) return false;

  for (size_t i = 0; i < len; i++) {
    if (pin[i] < '0' || pin[i] > '9') return false;
  }
  return true;
}

// The stream is read with esp_http_client, which only handles http(s) URLs.
// Anything else (a playlist, a local file, an HLS manifest) would fail at
// runtime with a much less obvious error.
static bool is_valid_url(const char *url) {
  if (url[0] == '\0') return false;
  size_t len = strlen(url);
  if (len >= APP_SETTINGS_URL_LEN) return false;
  return strncmp(url, "http://", 7) == 0 || strncmp(url, "https://", 8) == 0;
}

static void set_default_station(app_station_t *station) {
  memset(station, 0, sizeof(*station));
  strncpy(station->url, APP_STATION_URL, APP_SETTINGS_URL_LEN - 1);
  strncpy(station->name, APP_STATION_NAME, APP_SETTINGS_NAME_LEN - 1);
}

void app_settings_defaults(app_settings_t *out) {
  memset(out, 0, sizeof(*out));
  set_default_station(&out->stations[0]);
  out->station_count = 1;
  strncpy(out->wifi_ssid, APP_WIFI_SSID, APP_SETTINGS_SSID_LEN - 1);
  strncpy(out->wifi_password, APP_WIFI_PASSWORD,
          APP_SETTINGS_PASSWORD_LEN - 1);
  strncpy(out->target_name, APP_BT_TARGET_NAME, APP_SETTINGS_NAME_LEN - 1);
  strncpy(out->local_name, APP_BT_DEVICE_NAME, APP_SETTINGS_NAME_LEN - 1);
  strncpy(out->pin_code, APP_BT_PIN_CODE, APP_SETTINGS_PIN_LEN - 1);
  out->ssp_enabled = APP_BT_SSP_ENABLED != 0;
  out->buffer_ms = APP_BUFFER_MS;
}

// Returns false for values that cannot work, so a bad NVS entry or a crafted
// POST never reaches the audio path.
static void validate(app_settings_t *candidate, bool log_problems) {
  uint8_t count = candidate->station_count;
  if (count > APP_SETTINGS_MAX_STATIONS) count = APP_SETTINGS_MAX_STATIONS;

  // Drop stations that cannot be played and close the gaps.
  uint8_t kept = 0;
  for (uint8_t i = 0; i < count; i++) {
    app_station_t *station = &candidate->stations[i];
    force_terminator(station->url, APP_SETTINGS_URL_LEN);
    force_terminator(station->name, APP_SETTINGS_NAME_LEN);

    if (!is_valid_url(station->url)) {
      if (log_problems) {
        ESP_LOGW(TAG, "station %u has no http(s) URL, dropping it",
                 (unsigned)(i + 1));
      }
      continue;
    }
    if (kept != i) candidate->stations[kept] = *station;
    kept++;
  }

  if (kept == 0) {
    if (log_problems) ESP_LOGW(TAG, "no usable station left: using the default");
    set_default_station(&candidate->stations[0]);
    kept = 1;
  }
  candidate->station_count = kept;

  force_terminator(candidate->wifi_ssid, APP_SETTINGS_SSID_LEN);
  force_terminator(candidate->wifi_password, APP_SETTINGS_PASSWORD_LEN);
  force_terminator(candidate->target_name, APP_SETTINGS_NAME_LEN);
  force_terminator(candidate->local_name, APP_SETTINGS_NAME_LEN);
  force_terminator(candidate->pin_code, APP_SETTINGS_PIN_LEN);

  if (candidate->local_name[0] == '\0') {
    strncpy(candidate->local_name, APP_BT_DEVICE_NAME,
            APP_SETTINGS_NAME_LEN - 1);
    force_terminator(candidate->local_name, APP_SETTINGS_NAME_LEN);
  }

  if (!is_valid_pin(candidate->pin_code)) {
    if (log_problems) {
      ESP_LOGW(TAG, "PIN '%s' is not 1-16 digits, using the default",
               candidate->pin_code);
    }
    strncpy(candidate->pin_code, APP_BT_PIN_CODE, APP_SETTINGS_PIN_LEN - 1);
    force_terminator(candidate->pin_code, APP_SETTINGS_PIN_LEN);
  }

  // An empty password is allowed: it means either "not set yet" or an open
  // network. Anything else has to be a WPA2 passphrase.
  size_t password_len = strlen(candidate->wifi_password);
  if (password_len != 0 && (password_len < 8 || password_len > 63)) {
    if (log_problems) {
      ESP_LOGW(TAG, "Wi-Fi password must be 8-63 characters, clearing it");
    }
    candidate->wifi_password[0] = '\0';
  }

  if (candidate->buffer_ms < APP_SETTINGS_BUFFER_MIN_MS ||
      candidate->buffer_ms > APP_SETTINGS_BUFFER_MAX_MS) {
    if (log_problems) {
      ESP_LOGW(TAG, "buffer %" PRIu32 " ms out of range, using %d ms",
               candidate->buffer_ms, APP_BUFFER_MS);
    }
    candidate->buffer_ms = APP_BUFFER_MS;
  }
}

static void log_settings(const char *prefix) {
  uint8_t count = s_settings.station_count;

  ESP_LOGI(TAG,
           "%s: %u station(s), first '%s' (%s), Wi-Fi '%s', sink '%s', "
           "local '%s', PIN '%s', SSP %s, buffer %" PRIu32 " ms",
           prefix, (unsigned)count,
           s_settings.stations[0].name[0] ? s_settings.stations[0].name
                                          : "(unnamed)",
           s_settings.stations[0].url,
           s_settings.wifi_ssid[0] ? s_settings.wifi_ssid : "(not set)",
           s_settings.target_name[0] ? s_settings.target_name : "(any)",
           s_settings.local_name,
           s_settings.pin_code[0] ? s_settings.pin_code : "(library default)",
           s_settings.ssp_enabled ? "on" : "off", s_settings.buffer_ms);
}

// Keeps Wi-Fi, Bluetooth and audio settings; the old single station becomes
// the only entry of the new list.
static void migrate_v1(app_settings_t *out, const stored_blob_v1_t *old) {
  memset(out, 0, sizeof(*out));

  out->station_count = 1;
  strncpy(out->stations[0].url, old->settings.station_url,
          APP_SETTINGS_URL_LEN - 1);
  strncpy(out->stations[0].name, old->settings.station_name,
          APP_SETTINGS_NAME_LEN - 1);
  strncpy(out->wifi_ssid, old->settings.wifi_ssid, APP_SETTINGS_SSID_LEN - 1);
  strncpy(out->wifi_password, old->settings.wifi_password,
          APP_SETTINGS_PASSWORD_LEN - 1);
  strncpy(out->target_name, old->settings.target_name,
          APP_SETTINGS_NAME_LEN - 1);
  strncpy(out->local_name, old->settings.local_name, APP_SETTINGS_NAME_LEN - 1);
  strncpy(out->pin_code, old->settings.pin_code, APP_SETTINGS_PIN_LEN - 1);
  out->ssp_enabled = old->settings.ssp_enabled;
  out->buffer_ms = old->settings.buffer_ms;
}

void app_settings_load(void) {
  app_settings_defaults(&s_settings);

  esp_err_t err = nvs_flash_init();
  if (err != ESP_OK && err != ESP_ERR_NVS_NO_FREE_PAGES &&
      err != ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_LOGW(TAG, "nvs init failed (%s): using defaults", esp_err_to_name(err));
    s_loaded = true;
    return;
  }

  nvs_handle_t handle;
  if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
    ESP_LOGI(TAG, "no stored settings yet: writing defaults");
    app_settings_save(&s_settings);
    s_loaded = true;
    log_settings("defaults");
    return;
  }

  size_t length = sizeof(s_blob);
  err = nvs_get_blob(handle, NVS_KEY, &s_blob, &length);

  if (err != ESP_OK || length != sizeof(s_blob) ||
      s_blob.version != SETTINGS_VERSION) {
    size_t old_length = sizeof(s_old_blob);
    if (nvs_get_blob(handle, NVS_KEY, &s_old_blob, &old_length) == ESP_OK &&
        old_length == sizeof(s_old_blob) && s_old_blob.version == 1) {
      nvs_close(handle);
      migrate_v1(&s_settings, &s_old_blob);
      validate(&s_settings, true);
      app_settings_save(&s_settings);
      s_loaded = true;
      log_settings("migrated from the single-station layout");
      return;
    }

    nvs_close(handle);
    ESP_LOGI(TAG, "stored settings missing or stale: using defaults");
    app_settings_save(&s_settings);
    s_loaded = true;
    log_settings("defaults");
    return;
  }

  nvs_close(handle);

  s_settings = s_blob.settings;
  validate(&s_settings, true);
  s_loaded = true;
  log_settings("loaded");
}

const app_settings_t *app_settings_get(void) {
  if (!s_loaded) app_settings_load();
  return &s_settings;
}

bool app_settings_save(const app_settings_t *candidate) {
  if (candidate == NULL) return false;

  s_candidate = *candidate;
  validate(&s_candidate, true);

  esp_err_t err = nvs_flash_init();
  if (err != ESP_OK && err != ESP_ERR_NVS_NO_FREE_PAGES &&
      err != ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_LOGE(TAG, "nvs init failed: %s", esp_err_to_name(err));
    return false;
  }

  nvs_handle_t handle;
  err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "cannot open settings namespace: %s", esp_err_to_name(err));
    return false;
  }

  s_blob.version = SETTINGS_VERSION;
  s_blob.settings = s_candidate;
  err = nvs_set_blob(handle, NVS_KEY, &s_blob, sizeof(s_blob));
  if (err == ESP_OK) err = nvs_commit(handle);
  nvs_close(handle);

  if (err != ESP_OK) {
    ESP_LOGE(TAG, "cannot store settings: %s", esp_err_to_name(err));
    return false;
  }

  s_settings = s_candidate;
  s_loaded = true;
  log_settings("saved");
  return true;
}
