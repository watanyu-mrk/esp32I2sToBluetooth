#include "config_portal.h"

#include <string.h>

#include "cJSON.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "a2dp_source.h"
#include "app_settings.h"
#include "bt_scan.h"

static const char *TAG = "portal";

// Open (or WPA2) access point used only during configuration.
#define PORTAL_SSID "ESP32-A2DP-Setup"
#define PORTAL_PASSWORD "esp32cam"  // WPA2 needs at least 8 characters
#define PORTAL_CHANNEL 6
#define PORTAL_MAX_CLIENTS 2

// The page is embedded by CMake (EMBED_FILES in main/CMakeLists.txt).
extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[] asm("_binary_index_html_end");

static httpd_handle_t s_server;
static esp_netif_t *s_netif;
static bool s_wifi_started;
static esp_event_handler_instance_t s_wifi_handler;

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

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data) {
  (void)arg;
  (void)event_base;
  (void)event_data;

  if (event_id == WIFI_EVENT_AP_STACONNECTED) {
    ESP_LOGI(TAG, "client joined the config access point");
  } else if (event_id == WIFI_EVENT_AP_STADISCONNECTED) {
    ESP_LOGI(TAG, "client left the config access point");
  }
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

  cJSON *root = cJSON_CreateObject();
  if (root == NULL) return httpd_resp_send_500(req);

  cJSON_AddStringToObject(root, "bt", a2dp_source_state());
  cJSON_AddBoolToObject(root, "connected", a2dp_source_is_connected());
  cJSON_AddBoolToObject(root, "scanning", a2dp_source_is_scanning());
  // Bluetooth is shut down in config mode, so the page cannot ask for a new
  // search: the device list is whatever the boot-time discovery found.
  cJSON_AddBoolToObject(root, "bt_off", a2dp_source_is_shut_down());

  cJSON *json_settings = cJSON_AddObjectToObject(root, "settings");
  cJSON_AddStringToObject(json_settings, "target", settings->target_name);
  cJSON_AddStringToObject(json_settings, "local", settings->local_name);
  cJSON_AddBoolToObject(json_settings, "ssp", settings->ssp_enabled);
  cJSON_AddNumberToObject(json_settings, "input_rate", settings->input_rate);
  cJSON_AddNumberToObject(json_settings, "output_rate", settings->output_rate);
  cJSON_AddNumberToObject(json_settings, "buffer_ms", settings->buffer_ms);

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

static bool read_number_field(const cJSON *root, const char *key,
                              uint32_t *out) {
  const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
  if (!cJSON_IsNumber(item) || item->valuedouble < 0) return false;
  *out = (uint32_t)item->valuedouble;
  return true;
}

static esp_err_t settings_post(httpd_req_t *req) {
  if (req->content_len == 0 || req->content_len > 1024) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
    return ESP_FAIL;
  }

  char body[1025];
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

  app_settings_t next = *app_settings_get();
  copy_string_field(root, "target", next.target_name,
                    sizeof(next.target_name));
  copy_string_field(root, "local", next.local_name, sizeof(next.local_name));
  read_number_field(root, "input_rate", &next.input_rate);
  read_number_field(root, "buffer_ms", &next.buffer_ms);

  const cJSON *ssp = cJSON_GetObjectItemCaseSensitive(root, "ssp");
  if (cJSON_IsBool(ssp)) next.ssp_enabled = cJSON_IsTrue(ssp);

  cJSON_Delete(root);

  bool saved = app_settings_save(&next);

  httpd_resp_set_type(req, "application/json");
  httpd_resp_sendstr(req, saved ? "{\"ok\":true,\"reboot\":true}"
                                : "{\"ok\":false}");
  if (saved) schedule_reboot();
  return ESP_OK;
}

static esp_err_t rescan_post(httpd_req_t *req) {
  // With Bluetooth released there is nothing left that could discover devices,
  // so keep the list from the search that ran before the portal opened.
  if (!a2dp_source_is_shut_down()) bt_scan_clear();
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
  // The whole point of the portal is that only one phone talks to it, and the
  // Bluetooth stack is holding most of the heap: the default seven sessions
  // need a contiguous block that no longer exists by the time we get here.
  config.max_open_sockets = 2;
  config.backlog_conn = 2;
  config.max_resp_headers = 4;
  config.stack_size = 4096;
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

static bool start_wifi(void) {
  esp_netif_init();
  esp_err_t err = esp_event_loop_create_default();
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
    ESP_LOGE(TAG, "event loop: %s", esp_err_to_name(err));
    return false;
  }

  s_netif = esp_netif_create_default_wifi_ap();
  if (s_netif == NULL) {
    ESP_LOGE(TAG, "cannot create the AP network interface");
    return false;
  }

  wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
  err = esp_wifi_init(&init);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_wifi_init: %s", esp_err_to_name(err));
    return false;
  }

  esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                      wifi_event_handler, NULL,
                                      &s_wifi_handler);

  wifi_config_t wifi_config = {
      .ap =
          {
              .ssid = PORTAL_SSID,
              .password = PORTAL_PASSWORD,
              .ssid_len = sizeof(PORTAL_SSID) - 1,
              .channel = PORTAL_CHANNEL,
              .max_connection = PORTAL_MAX_CLIENTS,
              .authmode = WIFI_AUTH_WPA2_PSK,
          },
  };

  if (esp_wifi_set_mode(WIFI_MODE_AP) != ESP_OK ||
      esp_wifi_set_config(WIFI_IF_AP, &wifi_config) != ESP_OK) {
    ESP_LOGE(TAG, "cannot configure the access point");
    return false;
  }
  // No power saving: the page has to answer quickly while it is being used.
  esp_wifi_set_ps(WIFI_PS_NONE);

  err = esp_wifi_start();
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_wifi_start: %s", esp_err_to_name(err));
    return false;
  }

  s_wifi_started = true;
  ESP_LOGI(TAG, "access point '%s' up, open http://192.168.4.1/ (password '%s')",
           PORTAL_SSID, PORTAL_PASSWORD);
  return true;
}

bool config_portal_start(void) {
  if (s_server != NULL) return true;

  if (!start_wifi()) {
    config_portal_stop();
    return false;
  }
  if (!start_http_server()) {
    config_portal_stop();
    return false;
  }

  ESP_LOGI(TAG, "config portal ready (%u bytes free)", 
           (unsigned)esp_get_free_heap_size());
  return true;
}

void config_portal_stop(void) {
  if (s_server != NULL) {
    httpd_stop(s_server);
    s_server = NULL;
    ESP_LOGI(TAG, "http server stopped");
  }

  if (s_wifi_started) {
    esp_wifi_stop();
    esp_wifi_deinit();
    s_wifi_started = false;
    ESP_LOGI(TAG, "wi-fi stopped (%u bytes free)",
             (unsigned)esp_get_free_heap_size());
  }

  if (s_wifi_handler != NULL) {
    esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                          s_wifi_handler);
    s_wifi_handler = NULL;
  }

  if (s_netif != NULL) {
    esp_netif_destroy(s_netif);
    s_netif = NULL;
  }
}

bool config_portal_is_running(void) { return s_server != NULL; }
