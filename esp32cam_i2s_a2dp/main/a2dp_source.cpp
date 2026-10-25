#include "a2dp_source.h"

#include <string.h>

#include "BluetoothA2DPSource.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "app_config.h"
#include "app_settings.h"
#include "audio_buffer.h"
#include "bt_scan.h"

static const char *TAG = "a2dp_src";

static BluetoothA2DPSource s_source;
static bool s_started = false;
static bool s_shut_down = false;
static volatile bool s_scanning = false;

// The library only refreshes the cached audio state that get_audio_state()
// returns - and therefore that a2dp_source_is_streaming() depends on - inside
// its audio state event handler, and it only does so when a callback is
// registered. Without one the bridge stays "suspended" forever and never feeds
// any PCM, so this no-op handler is required even though it only logs.
static void on_audio_state_changed(esp_a2d_audio_state_t state, void *obj) {
  (void)obj;
  // In IDF 5.x ESP_A2D_AUDIO_STATE_STOPPED is an alias of ..._SUSPEND, so only
  // STARTED is worth telling apart.
  ESP_LOGI(TAG, "audio state: %s",
           state == ESP_A2D_AUDIO_STATE_STARTED ? "started"
                                                : "suspended/stopped");
}

// Called by the Bluetooth stack whenever it needs more PCM. It must return
// quickly: read what is buffered, pad the rest with silence, and never block.
static int32_t provide_audio(uint8_t *data, int32_t len) {
  if (data == nullptr || len <= 0) return 0;

  size_t frames = (size_t)len / APP_BYTES_PER_FRAME;
  if (frames == 0) return 0;

  size_t read = audio_buffer_read(reinterpret_cast<int16_t *>(data), frames);
  if (read < frames) {
    memset(data + read * APP_BYTES_PER_FRAME, 0,
           (frames - read) * APP_BYTES_PER_FRAME);
    audio_buffer_record_underflow(frames - read);
  }
  // Always report the full length: a short return would make the stack send a
  // truncated media packet, which is audible.
  return len;
}

// Called for every discovered device with an audio service. It records the
// device for the config page and decides whether it is the configured sink:
// an empty target name accepts the first one, otherwise the name has to start
// with the configured text (the same rule the library applies by itself).
static bool on_sink_found(const char *name, esp_bd_addr_t address, int rssi) {
  bt_scan_record(name, address, rssi);

  const char *target = app_settings_get()->target_name;
  if (target[0] == '\0') return true;
  if (name == nullptr) return false;
  return strncmp(name, target, strlen(target)) == 0;
}

static void on_discovery_mode(esp_bt_gap_discovery_state_t state) {
  s_scanning = state == ESP_BT_GAP_DISCOVERY_STARTED;
  ESP_LOGI(TAG, "discovery %s", s_scanning ? "started" : "stopped");
}

// The library stores the last peer address in NVS and reconnects to it on
// start() before any discovery happens. With a speaker name configured, a
// stale entry (another speaker, or an old pairing) would make the bridge
// retry that address forever. Clearing it forces a discovery of the
// configured sink; the address is stored again once connected, so
// reconnection after a dropout keeps working.
static void clear_stored_peer(void) {
  esp_err_t err = nvs_flash_init();
  if (err != ESP_OK && err != ESP_ERR_NVS_NO_FREE_PAGES &&
      err != ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_LOGW(TAG, "nvs init failed: %s", esp_err_to_name(err));
    return;
  }

  nvs_handle_t handle;
  if (nvs_open("connected_bda", NVS_READWRITE, &handle) != ESP_OK) {
    return;  // namespace does not exist yet: nothing stored
  }
  if (nvs_erase_all(handle) == ESP_OK) {
    nvs_commit(handle);
    ESP_LOGI(TAG, "cleared the stored peer address");
  }
  nvs_close(handle);
}

void a2dp_source_start(void) {
  if (s_started) return;
  s_started = true;

  const app_settings_t *settings = app_settings_get();

  s_source.set_local_name(settings->local_name);
  s_source.set_auto_reconnect(true, APP_BT_RECONNECT_TRIES);
  s_source.set_data_callback(provide_audio);

  // Required for is_streaming() to ever become true (see the handler above).
  s_source.set_on_audio_state_changed(on_audio_state_changed);

  // Both callbacks are also what turns the plain discovery into a usable
  // "which speakers are nearby" list for the config page.
  s_source.set_ssid_callback(on_sink_found);
  s_source.set_discovery_mode_callback(on_discovery_mode);

  // SSP must be configured before start(): the library applies the IO
  // capability to Bluedroid while bringing the stack up. Many modern speakers
  // refuse the legacy PIN handshake and only pair over Secure Simple Pairing.
  s_source.set_ssp_enabled(settings->ssp_enabled);
  ESP_LOGI(TAG, "pairing: SSP %s",
           settings->ssp_enabled ? "enabled (numeric value auto-confirmed)"
                                 : "disabled (legacy PIN only)");

  if (settings->target_name[0] != '\0') {
    clear_stored_peer();
    ESP_LOGI(TAG, "local name '%s', looking for sink '%s'",
             settings->local_name, settings->target_name);
    s_source.start(settings->target_name);
  } else {
    ESP_LOGI(TAG, "local name '%s', connecting to the first A2DP sink",
             settings->local_name);
    s_source.start();
  }
}

void a2dp_source_stop(void) {
  if (!s_started) return;

  ESP_LOGW(TAG, "shutting Bluetooth down to free the radio and the heap");
  // end(true) disables and deinitialises Bluedroid and the controller and
  // releases the Classic BT memory - that is the memory the Wi-Fi stack needs
  // for the config portal. It also makes Bluetooth unusable until the next
  // boot, which is fine: the portal only comes up when the bridge has given up
  // on finding a speaker.
  s_source.end(true);
  s_started = false;
  s_shut_down = true;
  s_scanning = false;
  ESP_LOGI(TAG, "Bluetooth released (%u bytes free)",
           (unsigned)esp_get_free_heap_size());
}

bool a2dp_source_is_shut_down(void) { return s_shut_down; }

bool a2dp_source_is_connected(void) {
  if (!s_started) return false;
  return s_source.is_connected();
}

bool a2dp_source_is_active(void) {
  if (!s_started) return false;
  return s_source.get_connection_state() !=
         ESP_A2D_CONNECTION_STATE_DISCONNECTED;
}

bool a2dp_source_is_scanning(void) {
  if (!s_started) return false;
  return s_scanning || s_source.is_discovery_active();
}

bool a2dp_source_is_streaming(void) {
  if (!s_started) return false;
  return s_source.is_connected() &&
         s_source.get_audio_state() == ESP_A2D_AUDIO_STATE_STARTED;
}

const char *a2dp_source_state(void) {
  if (s_shut_down) return "Off (config mode)";
  if (!s_started) return "Idle";

  switch (s_source.get_connection_state()) {
    case ESP_A2D_CONNECTION_STATE_DISCONNECTED:
      return "Disconnected";
    case ESP_A2D_CONNECTION_STATE_CONNECTING:
      return "Connecting";
    case ESP_A2D_CONNECTION_STATE_CONNECTED:
      switch (s_source.get_audio_state()) {
        case ESP_A2D_AUDIO_STATE_STARTED:
          return "Streaming";
        case ESP_A2D_AUDIO_STATE_SUSPEND:
          return "Connected (suspended)";
        default:
          return "Connected";
      }
    case ESP_A2D_CONNECTION_STATE_DISCONNECTING:
      return "Disconnecting";
  }
  return "Unknown";
}
