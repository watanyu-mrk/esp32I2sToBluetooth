#include "a2dp_source.h"

#include <string.h>

#include "BluetoothA2DPSource.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "app_config.h"
#include "audio_buffer.h"

static const char *TAG = "a2dp_src";

static BluetoothA2DPSource s_source;
static bool s_started = false;

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

// Used when no target name is configured.
static bool accept_any_sink(const char *name, esp_bd_addr_t address, int rssi) {
  (void)address;
  ESP_LOGI(TAG, "sink candidate '%s' (rssi %d)", name != nullptr ? name : "?",
           rssi);
  return true;
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

void a2dp_source_start(const char *target_name) {
  if (s_started) return;
  s_started = true;

  s_source.set_local_name(APP_BT_DEVICE_NAME);
  s_source.set_auto_reconnect(true, APP_BT_RECONNECT_TRIES);
  s_source.set_data_callback(provide_audio);

  if (target_name != nullptr && target_name[0] != '\0') {
    clear_stored_peer();
    ESP_LOGI(TAG, "local name '%s', looking for sink '%s'", APP_BT_DEVICE_NAME,
             target_name);
    s_source.start(target_name);
  } else {
    ESP_LOGI(TAG, "local name '%s', connecting to the first A2DP sink",
             APP_BT_DEVICE_NAME);
    s_source.set_ssid_callback(accept_any_sink);
    s_source.start();
  }
}

bool a2dp_source_is_streaming(void) {
  if (!s_started) return false;
  return s_source.is_connected() &&
         s_source.get_audio_state() == ESP_A2D_AUDIO_STATE_STARTED;
}

const char *a2dp_source_state(void) {
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
