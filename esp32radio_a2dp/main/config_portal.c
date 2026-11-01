#include "config_portal.h"

#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "a2dp_source.h"
#include "app_config.h"
#include "app_settings.h"
#include "audio_buffer.h"
#include "bt_scan.h"
#include "radio_stream.h"
#include "wifi_link.h"

static const char *TAG = "portal";

// The page is embedded by CMake (EMBED_FILES in main/CMakeLists.txt).
extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[] asm("_binary_index_html_end");

static httpd_handle_t s_server;

// One POST carries the whole settings form: every station plus the other
// fields, JSON-encoded. With the full station list that is several kilobytes,
// more than internal RAM can spare, so the buffer lives in PSRAM.
#define SETTINGS_BODY_MAX                                    \
  (APP_SETTINGS_MAX_STATIONS *                               \
       (APP_SETTINGS_URL_LEN + APP_SETTINGS_NAME_LEN + 64) + \
   512)
static char *s_body;
// The settings struct is several kilobytes once the station list is full, so
// the working copy for a POST lives in PSRAM: as a local it overflowed the
// 8 KB httpd task stack and killed the server.
static app_settings_t *s_next;

static void reboot_task(void *arg) {
  (void)arg;
  // Give the response time to leave the socket before the socket disappears.
  vTaskDelay(pdMS_TO_TICKS(1200));
  ESP_LOGW(TAG, "restarting to apply the new settings");
  esp_restart();
}

static void schedule_reboot(void) {
  xTaskCreate(reboot_task, "cfg_reboot", 2048, NULL, 5, NULL);
}

// ---------------------------------------------------------------------------
// HTTP handlers
// ---------------------------------------------------------------------------

static esp_err_t page_get(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/html; charset=utf-8");
  size_t length = (size_t)(index_html_end - index_html_start);
  return httpd_resp_send(req, (const char *)index_html_start, length);
}

static esp_err_t state_get(httpd_req_t *req) {
  const app_settings_t *settings = app_settings_get();
  radio_stream_info_t radio;
  radio_stream_get_info(&radio);
  audio_buffer_stats_t buffer = audio_buffer_get_stats();

  cJSON *root = cJSON_CreateObject();
  if (root == NULL) return httpd_resp_send_500(req);

  cJSON_AddStringToObject(root, "bt", a2dp_source_state());
  cJSON_AddBoolToObject(root, "connected", a2dp_source_is_connected());
  cJSON_AddBoolToObject(root, "scanning", a2dp_source_is_scanning());
  cJSON_AddNumberToObject(root, "max_stations", APP_SETTINGS_MAX_STATIONS);

  cJSON *wifi = cJSON_AddObjectToObject(root, "wifi");
  cJSON_AddStringToObject(wifi, "state", wifi_link_state_text());
  cJSON_AddStringToObject(wifi, "ip", wifi_link_ip());
  cJSON_AddBoolToObject(wifi, "ap", wifi_link_ap_active());
  cJSON_AddNumberToObject(wifi, "rssi", wifi_link_rssi());

  cJSON *stream = cJSON_AddObjectToObject(root, "radio");
  cJSON_AddStringToObject(stream, "state", radio.state);
  cJSON_AddNumberToObject(stream, "station", radio.station_index);
  cJSON_AddNumberToObject(stream, "count", radio.station_count);
  cJSON_AddStringToObject(stream, "station_name", radio.station_name);
  cJSON_AddStringToObject(stream, "content", radio.content_type);
  cJSON_AddNumberToObject(stream, "rate", radio.sample_rate);
  cJSON_AddNumberToObject(stream, "kbps", radio.bitrate);
  cJSON_AddNumberToObject(stream, "channels", radio.channels);
  cJSON_AddNumberToObject(stream, "dropped", radio.dropped_frames);
  cJSON_AddNumberToObject(stream, "errors", radio.decode_errors);
  cJSON_AddNumberToObject(stream, "failures", radio.failures);
  cJSON_AddNumberToObject(stream, "bytes", radio.bytes);

  cJSON *buffered = cJSON_AddObjectToObject(root, "buffer");
  cJSON_AddNumberToObject(buffered, "frames", (double)buffer.available_frames);
  cJSON_AddNumberToObject(buffered, "capacity", (double)buffer.capacity_frames);
  cJSON_AddNumberToObject(buffered, "underflow",
                          (double)buffer.underflow_frames);
  cJSON_AddNumberToObject(buffered, "dropped", (double)buffer.dropped_frames);

  cJSON *json_settings = cJSON_AddObjectToObject(root, "settings");
  cJSON *stations = cJSON_AddArrayToObject(json_settings, "stations");
  for (uint8_t i = 0; i < settings->station_count; i++) {
    cJSON *station = cJSON_CreateObject();
    if (station == NULL) break;
    cJSON_AddStringToObject(station, "url", settings->stations[i].url);
    cJSON_AddStringToObject(station, "name", settings->stations[i].name);
    cJSON_AddItemToArray(stations, station);
  }
  cJSON_AddStringToObject(json_settings, "ssid", settings->wifi_ssid);
  cJSON_AddStringToObject(json_settings, "password",
                          settings->wifi_password);
  cJSON_AddStringToObject(json_settings, "target", settings->target_name);
  cJSON_AddStringToObject(json_settings, "local", settings->local_name);
  cJSON_AddStringToObject(json_settings, "pin", settings->pin_code);
  cJSON_AddBoolToObject(json_settings, "ssp", settings->ssp_enabled);
  cJSON_AddNumberToObject(json_settings, "buffer_ms", settings->buffer_ms);
  cJSON_AddNumberToObject(json_settings, "volume", settings->volume);

  cJSON *devices = cJSON_AddArrayToObject(root, "devices");
  bt_scan_device_t found[BT_SCAN_MAX_DEVICES];
  size_t count = bt_scan_list(found, BT_SCAN_MAX_DEVICES);
  for (size_t i = 0; i < count; i++) {
    cJSON *device = cJSON_CreateObject();
    if (device == NULL) break;
    cJSON_AddStringToObject(device, "name", found[i].name);
    cJSON_AddStringToObject(device, "addr", found[i].address);
    cJSON_AddNumberToObject(device, "rssi", found[i].rssi);
    cJSON_AddItemToArray(devices, device);
  }

  char *text = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  if (text == NULL) return httpd_resp_send_500(req);

  httpd_resp_set_type(req, "application/json");
  esp_err_t err = httpd_resp_sendstr(req, text);
  free(text);
  return err;
}

static bool copy_string_field(const cJSON *root, const char *key, char *out,
                              size_t size) {
  const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
  if (!cJSON_IsString(item) || item->valuestring == NULL) return false;
  strncpy(out, item->valuestring, size - 1);
  out[size - 1] = '\0';
  return true;
}

// Reads the station list. A missing or empty array keeps the stored list, so
// an older cached page cannot wipe it.
static void read_stations(const cJSON *root, app_settings_t *out) {
  const cJSON *list = cJSON_GetObjectItemCaseSensitive(root, "stations");
  if (!cJSON_IsArray(list)) return;

  uint8_t count = 0;
  const cJSON *item = NULL;
  cJSON_ArrayForEach(item, list) {
    if (count >= APP_SETTINGS_MAX_STATIONS) break;

    const cJSON *url = cJSON_GetObjectItemCaseSensitive(item, "url");
    if (!cJSON_IsString(url) || url->valuestring == NULL) continue;

    app_station_t *station = &out->stations[count];
    memset(station, 0, sizeof(*station));
    strncpy(station->url, url->valuestring, APP_SETTINGS_URL_LEN - 1);

    const cJSON *name = cJSON_GetObjectItemCaseSensitive(item, "name");
    if (cJSON_IsString(name) && name->valuestring != NULL) {
      strncpy(station->name, name->valuestring, APP_SETTINGS_NAME_LEN - 1);
    }
    count++;
  }

  if (count > 0) out->station_count = count;
}

static bool read_number_field(const cJSON *root, const char *key,
                              uint32_t *out) {
  const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
  if (!cJSON_IsNumber(item) || item->valuedouble < 0) return false;
  *out = (uint32_t)item->valuedouble;
  return true;
}

static esp_err_t settings_post(httpd_req_t *req) {
  // The page sends every field at once: up to APP_SETTINGS_MAX_STATIONS full
  // URLs plus the Wi-Fi/Bluetooth fields.
  if (req->content_len == 0 || req->content_len >= SETTINGS_BODY_MAX) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
    return ESP_FAIL;
  }

  if (s_body == NULL) {
    s_body = heap_caps_malloc(SETTINGS_BODY_MAX, MALLOC_CAP_SPIRAM);
    if (s_body == NULL) s_body = malloc(SETTINGS_BODY_MAX);
  }
  char *body = s_body;
  if (body == NULL) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
    return ESP_FAIL;
  }

  size_t received = 0;
  while (received < req->content_len) {
    int read = httpd_req_recv(req, body + received, req->content_len - received);
    if (read <= 0) {
      if (read == HTTPD_SOCK_ERR_TIMEOUT) continue;
      httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "short body");
      return ESP_FAIL;
    }
    received += (size_t)read;
  }
  body[received] = '\0';

  cJSON *root = cJSON_Parse(body);
  if (root == NULL) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid json");
    return ESP_FAIL;
  }

  if (s_next == NULL) {
    s_next = heap_caps_malloc(sizeof(*s_next), MALLOC_CAP_SPIRAM);
    if (s_next == NULL) s_next = malloc(sizeof(*s_next));
  }
  if (s_next == NULL) {
    cJSON_Delete(root);
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
    return ESP_FAIL;
  }

  app_settings_t *next = s_next;
  *next = *app_settings_get();
  read_stations(root, next);
  copy_string_field(root, "ssid", next->wifi_ssid, sizeof(next->wifi_ssid));
  copy_string_field(root, "password", next->wifi_password,
                    sizeof(next->wifi_password));
  copy_string_field(root, "target", next->target_name,
                    sizeof(next->target_name));
  copy_string_field(root, "local", next->local_name, sizeof(next->local_name));
  copy_string_field(root, "pin", next->pin_code, sizeof(next->pin_code));
  read_number_field(root, "buffer_ms", &next->buffer_ms);

  // The volume goes through a 32-bit read first so a crafted value cannot
  // wrap around into the uint8_t field.
  uint32_t volume = next->volume;
  if (read_number_field(root, "volume", &volume)) {
    if (volume > APP_SETTINGS_VOLUME_MAX) volume = APP_SETTINGS_VOLUME_MAX;
    next->volume = (uint8_t)volume;
  }

  const cJSON *ssp = cJSON_GetObjectItemCaseSensitive(root, "ssp");
  if (cJSON_IsBool(ssp)) next->ssp_enabled = cJSON_IsTrue(ssp);

  cJSON_Delete(root);

  bool saved = app_settings_save(next);

  httpd_resp_set_type(req, "application/json");
  httpd_resp_sendstr(req, saved ? "{\"ok\":true,\"reboot\":true}"
                                : "{\"ok\":false}");
  if (saved) schedule_reboot();
  return ESP_OK;
}

static esp_err_t rescan_post(httpd_req_t *req) {
  // The A2DP source keeps scanning while it hunts for its speaker; clearing
  // the cache just resets the list the page shows.
  bt_scan_clear();
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t reboot_post(httpd_req_t *req) {
  httpd_resp_set_type(req, "application/json");
  httpd_resp_sendstr(req, "{\"ok\":true,\"reboot\":true}");
  schedule_reboot();
  return ESP_OK;
}

static bool start_http_server(void) {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.max_uri_handlers = 8;
  // The status endpoint is polled while audio streams, so keep the number of
  // sockets and the per-session buffers small.
  config.max_open_sockets = 3;
  config.backlog_conn = 3;
  config.max_resp_headers = 4;
  // The settings POST parses a body of up to 3.5 KB on this stack.
  config.stack_size = 8192;
  config.lru_purge_enable = true;

  if (httpd_start(&s_server, &config) != ESP_OK) {
    ESP_LOGE(TAG, "cannot start the http server");
    s_server = NULL;
    return false;
  }

  const httpd_uri_t routes[] = {
      {.uri = "/", .method = HTTP_GET, .handler = page_get},
      {.uri = "/api/state", .method = HTTP_GET, .handler = state_get},
      {.uri = "/api/settings", .method = HTTP_POST, .handler = settings_post},
      {.uri = "/api/rescan", .method = HTTP_POST, .handler = rescan_post},
      {.uri = "/api/reboot", .method = HTTP_POST, .handler = reboot_post},
  };
  for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
    esp_err_t err = httpd_register_uri_handler(s_server, &routes[i]);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "cannot register %s: %s", routes[i].uri,
               esp_err_to_name(err));
      httpd_stop(s_server);
      s_server = NULL;
      return false;
    }
  }
  return true;
}

bool config_portal_start(void) {
  if (s_server != NULL) return true;

  if (esp_get_free_heap_size() < APP_PORTAL_MIN_HEAP_BYTES) {
    ESP_LOGW(TAG, "only %u bytes free: starting the config server anyway",
             (unsigned)esp_get_free_heap_size());
  }

  if (!start_http_server()) return false;

  ESP_LOGI(TAG, "config page ready on port 80 (%u bytes free)",
           (unsigned)esp_get_free_heap_size());
  return true;
}

bool config_portal_is_running(void) { return s_server != NULL; }
