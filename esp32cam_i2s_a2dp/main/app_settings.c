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
#define SETTINGS_VERSION 2

// The legacy SBC path in Bluedroid feeds its encoder with a fixed 44100 Hz PCM
// stream (btc_a2dp_source.c: media_feeding.cfg.pcm.sampling_freq = 44100), so
// the bridge must hand it 44100 Hz no matter what the sink negotiates.
#define A2DP_PCM_RATE 44100

typedef struct {
  uint8_t version;
  app_settings_t settings;
} stored_blob_t;

static app_settings_t s_settings;
static bool s_loaded;

static const uint32_t kValidInputRates[] = {44100, 48000};

static bool is_valid_input_rate(uint32_t rate) {
  for (size_t i = 0; i < sizeof(kValidInputRates) / sizeof(kValidInputRates[0]);
       i++) {
    if (kValidInputRates[i] == rate) return true;
  }
  return false;
}

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

void app_settings_defaults(app_settings_t *out) {
  memset(out, 0, sizeof(*out));
  strncpy(out->target_name, APP_BT_TARGET_NAME, APP_SETTINGS_NAME_LEN - 1);
  strncpy(out->local_name, APP_BT_DEVICE_NAME, APP_SETTINGS_NAME_LEN - 1);
  strncpy(out->pin_code, APP_BT_PIN_CODE, APP_SETTINGS_PIN_LEN - 1);
  out->ssp_enabled = APP_BT_SSP_ENABLED != 0;
  out->input_rate = APP_INPUT_SAMPLE_RATE;
  out->output_rate = A2DP_PCM_RATE;
  out->buffer_ms = APP_BUFFER_MS;
}

// Returns false for values that cannot work, so a bad NVS entry or a crafted
// POST never reaches the audio path.
static bool validate(app_settings_t *candidate, bool log_problems) {
  force_terminator(candidate->target_name, APP_SETTINGS_NAME_LEN);
  force_terminator(candidate->local_name, APP_SETTINGS_NAME_LEN);
  force_terminator(candidate->pin_code, APP_SETTINGS_PIN_LEN);

  if (candidate->local_name[0] == '\0') {
    strncpy(candidate->local_name, APP_BT_DEVICE_NAME, APP_SETTINGS_NAME_LEN - 1);
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

  if (!is_valid_input_rate(candidate->input_rate)) {
    if (log_problems) {
      ESP_LOGW(TAG, "unsupported input rate %" PRIu32 ", using %d",
               candidate->input_rate, APP_INPUT_SAMPLE_RATE);
    }
    candidate->input_rate = APP_INPUT_SAMPLE_RATE;
  }

  if (candidate->output_rate != A2DP_PCM_RATE) {
    if (log_problems) {
      ESP_LOGW(TAG,
               "A2DP runs at %d Hz (fixed by the Bluetooth SBC encoder), "
               "ignoring output rate %" PRIu32,
               A2DP_PCM_RATE, candidate->output_rate);
    }
    candidate->output_rate = A2DP_PCM_RATE;
  }

  if (candidate->buffer_ms < 50 || candidate->buffer_ms > 2000) {
    if (log_problems) {
      ESP_LOGW(TAG, "buffer %" PRIu32 " ms out of range, using %d ms",
               candidate->buffer_ms, APP_BUFFER_MS);
    }
    candidate->buffer_ms = APP_BUFFER_MS;
  }

  return true;
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
    return;
  }

  stored_blob_t blob;
  size_t length = sizeof(blob);
  err = nvs_get_blob(handle, NVS_KEY, &blob, &length);
  nvs_close(handle);

  if (err != ESP_OK || length != sizeof(blob) ||
      blob.version != SETTINGS_VERSION) {
    ESP_LOGI(TAG, "stored settings missing or stale: using defaults");
    app_settings_save(&s_settings);
    s_loaded = true;
    return;
  }

  app_settings_t loaded = blob.settings;
  validate(&loaded, true);
  s_settings = loaded;
  s_loaded = true;

  ESP_LOGI(TAG,
           "loaded: sink '%s', local '%s', PIN '%s', SSP %s, I2S %" PRIu32
           " Hz, A2DP %" PRIu32 " Hz, buffer %" PRIu32 " ms",
           s_settings.target_name[0] ? s_settings.target_name : "(any)",
           s_settings.local_name,
           s_settings.pin_code[0] ? s_settings.pin_code : "(library default)",
           s_settings.ssp_enabled ? "on" : "off",
           s_settings.input_rate, s_settings.output_rate, s_settings.buffer_ms);
}

const app_settings_t *app_settings_get(void) {
  if (!s_loaded) app_settings_load();
  return &s_settings;
}

bool app_settings_save(const app_settings_t *candidate) {
  if (candidate == NULL) return false;

  app_settings_t copy = *candidate;
  validate(&copy, true);

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

  stored_blob_t blob = {
      .version = SETTINGS_VERSION,
      .settings = copy,
  };
  err = nvs_set_blob(handle, NVS_KEY, &blob, sizeof(blob));
  if (err == ESP_OK) err = nvs_commit(handle);
  nvs_close(handle);

  if (err != ESP_OK) {
    ESP_LOGE(TAG, "cannot store settings: %s", esp_err_to_name(err));
    return false;
  }

  s_settings = copy;
  s_loaded = true;
  ESP_LOGI(TAG,
           "saved: sink '%s', local '%s', PIN '%s', SSP %s, I2S %" PRIu32
           " Hz, buffer %" PRIu32 " ms",
           s_settings.target_name[0] ? s_settings.target_name : "(any)",
           s_settings.local_name,
           s_settings.pin_code[0] ? s_settings.pin_code : "(library default)",
           s_settings.ssp_enabled ? "on" : "off",
           s_settings.input_rate, s_settings.buffer_ms);
  return true;
}
