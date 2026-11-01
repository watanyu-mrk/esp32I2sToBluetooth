#include "wifi_link.h"

#include <stdio.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_config.h"
#include "app_settings.h"

static const char *TAG = "wifi";

#define JOIN_RETRY_MS 3000
#define RSSI_REFRESH_MS 5000

typedef enum {
  LINK_OFF = 0,
  LINK_WAITING,    // no SSID configured
  LINK_CONNECTING,
  LINK_CONNECTED,
} link_state_t;

static esp_netif_t *s_sta_netif;
static esp_netif_t *s_ap_netif;
static bool s_wifi_started;
static bool s_ap_active;
static volatile bool s_connected;
// Associated with the AP. Separate from s_connected, which waits for DHCP:
// asking for a new connection in that window aborts the DHCP handshake.
static volatile bool s_sta_up;
static char s_ip[16];
static int s_rssi;
static link_state_t s_state = LINK_OFF;

// When the station lost its address; 0 while it is connected.
static int64_t s_down_since_us;
static int64_t s_last_retry_us;
static int64_t s_last_rssi_us;

static esp_event_handler_instance_t s_wifi_handler;
static esp_event_handler_instance_t s_ip_handler;

static void set_state(link_state_t state) {
  if (s_state == state) return;
  s_state = state;
  ESP_LOGI(TAG, "state: %s", wifi_link_state_text());
}

// ---------------------------------------------------------------------------
// Setup access point
// ---------------------------------------------------------------------------

static bool start_ap(void) {
  if (s_ap_active) return true;

  if (s_ap_netif == NULL) {
    s_ap_netif = esp_netif_create_default_wifi_ap();
    if (s_ap_netif == NULL) {
      ESP_LOGE(TAG, "cannot create the AP network interface");
      return false;
    }
  }

  // APSTA keeps the station interface alive: the board keeps retrying the
  // configured network (and streaming, if it ever joins) while the page is
  // reachable on 192.168.4.1.
  esp_err_t err = esp_wifi_set_mode(s_sta_netif != NULL ? WIFI_MODE_APSTA
                                                       : WIFI_MODE_AP);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_wifi_set_mode(APSTA): %s", esp_err_to_name(err));
    return false;
  }

  wifi_config_t config = {
      .ap =
          {
              .ssid = APP_WIFI_AP_SSID,
              .password = APP_WIFI_AP_PASSWORD,
              .ssid_len = sizeof(APP_WIFI_AP_SSID) - 1,
              .channel = APP_WIFI_AP_CHANNEL,
              .max_connection = APP_WIFI_AP_MAX_CLIENTS,
              .authmode = WIFI_AUTH_WPA2_PSK,
          },
  };

  err = esp_wifi_set_config(WIFI_IF_AP, &config);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "cannot configure the access point: %s",
             esp_err_to_name(err));
    return false;
  }

  s_ap_active = true;
  ESP_LOGW(TAG,
           "setup access point '%s' up: connect to it and open "
           "http://192.168.4.1/ (password '%s')",
           APP_WIFI_AP_SSID, APP_WIFI_AP_PASSWORD);
  return true;
}

// Closes the setup AP once the station has an address. The AP shares the 2.4
// GHz radio with the station and the Bluetooth link, so leaving it up costs
// audio time slices; the page stays reachable on the station's own IP.
static void close_ap(void) {
  if (!s_ap_active) return;

  esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "cannot close the setup AP: %s", esp_err_to_name(err));
    return;
  }

  s_ap_active = false;
  ESP_LOGI(TAG, "station is up: setup access point closed");
}

// ---------------------------------------------------------------------------
// Event handling
// ---------------------------------------------------------------------------

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id,
                               void *data) {
  (void)arg;
  (void)base;
  (void)data;

  switch (id) {
    case WIFI_EVENT_STA_START:
      ESP_LOGI(TAG, "station started, joining '%s'",
               app_settings_get()->wifi_ssid);
      esp_wifi_connect();
      break;

    case WIFI_EVENT_STA_CONNECTED:
      s_sta_up = true;
      s_last_retry_us = esp_timer_get_time();
      ESP_LOGI(TAG, "associated with '%s', waiting for an IP address",
               app_settings_get()->wifi_ssid);
      break;

    case WIFI_EVENT_STA_DISCONNECTED:
      s_sta_up = false;
      s_connected = false;
      if (s_down_since_us == 0) s_down_since_us = esp_timer_get_time();
      s_rssi = 0;
      s_ip[0] = '\0';
      // The retry itself is paced by wifi_link_poll(), so a wrong password
      // cannot turn into a connect storm.
      if (s_state == LINK_CONNECTED) ESP_LOGW(TAG, "connection lost");
      set_state(LINK_CONNECTING);
      s_last_retry_us = esp_timer_get_time();
      break;

    case WIFI_EVENT_AP_STACONNECTED:
      ESP_LOGI(TAG, "client joined the setup access point");
      break;

    case WIFI_EVENT_AP_STADISCONNECTED:
      ESP_LOGI(TAG, "client left the setup access point");
      break;

    default:
      break;
  }
}

static void ip_event_handler(void *arg, esp_event_base_t base, int32_t id,
                             void *data) {
  (void)arg;
  (void)base;

  if (id != IP_EVENT_STA_GOT_IP) return;

  const ip_event_got_ip_t *event = (const ip_event_got_ip_t *)data;
  snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&event->ip_info.ip));
  s_down_since_us = 0;
  s_connected = true;
  set_state(LINK_CONNECTED);
  ESP_LOGI(TAG, "joined '%s', IP %s (open http://%s/ for the config page)",
           app_settings_get()->wifi_ssid, s_ip, s_ip);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

static bool start_station(void) {
  const app_settings_t *settings = app_settings_get();

  // esp_wifi_init() leaves the mode at WIFI_MODE_NULL, and every
  // esp_wifi_set_config() is rejected with ESP_ERR_WIFI_MODE until a mode is
  // selected.
  esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_wifi_set_mode(STA): %s", esp_err_to_name(err));
    return false;
  }

  wifi_config_t config = {0};
  strncpy((char *)config.sta.ssid, settings->wifi_ssid,
          sizeof(config.sta.ssid) - 1);
  strncpy((char *)config.sta.password, settings->wifi_password,
          sizeof(config.sta.password) - 1);
  // WPA2_PSK also accepts WPA/WPA2 mixed networks; open networks are handled
  // because esp_wifi downgrades the threshold when the password is empty.
  config.sta.threshold.authmode = settings->wifi_password[0] == '\0'
                                     ? WIFI_AUTH_OPEN
                                     : WIFI_AUTH_WPA_PSK;
  config.sta.pmf_cfg.capable = true;
  config.sta.pmf_cfg.required = false;

  err = esp_wifi_set_config(WIFI_IF_STA, &config);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "cannot configure the station: %s", esp_err_to_name(err));
    return false;
  }

  // No modem sleep: the stream must not wait for a wake-up window, and the
  // coexistence scheduler already shares the radio with Bluetooth.
  esp_wifi_set_ps(WIFI_PS_NONE);

  err = esp_wifi_start();
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_wifi_start: %s", esp_err_to_name(err));
    return false;
  }

  s_wifi_started = true;
  return true;
}

// Configures the setup AP and starts the driver. Used both when no network is
// configured and as the fallback when the station cannot be started, so the
// page stays reachable either way.
static bool bring_up_ap(void) {
  if (!start_ap()) {
    ESP_LOGE(TAG, "setup access point failed");
    return false;
  }

  esp_err_t err = esp_wifi_start();
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_wifi_start: %s", esp_err_to_name(err));
    return false;
  }

  s_wifi_started = true;
  return true;
}

bool wifi_link_start(void) {
  if (s_wifi_started) return true;

  const app_settings_t *settings = app_settings_get();

  esp_err_t err = esp_netif_init();
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_netif_init: %s", esp_err_to_name(err));
    return false;
  }
  err = esp_event_loop_create_default();
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
    ESP_LOGE(TAG, "event loop: %s", esp_err_to_name(err));
    return false;
  }

  bool has_network = settings->wifi_ssid[0] != '\0';
  if (has_network) {
    s_sta_netif = esp_netif_create_default_wifi_sta();
    if (s_sta_netif == NULL) {
      ESP_LOGE(TAG, "cannot create the station network interface");
      return false;
    }
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
  esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                      ip_event_handler, NULL, &s_ip_handler);

  s_down_since_us = esp_timer_get_time();

  if (!has_network) {
    // Nothing to join: the board exists to be configured.
    ESP_LOGW(TAG, "no Wi-Fi network configured, starting the setup AP");
    set_state(LINK_WAITING);
    return bring_up_ap();
  }

  set_state(LINK_CONNECTING);
  if (start_station()) return true;

  ESP_LOGW(TAG, "cannot start the station, falling back to the setup AP");
  return bring_up_ap();
}

void wifi_link_poll(void) {
  if (!s_wifi_started) return;

  int64_t now = esp_timer_get_time();

  if (s_connected) {
    if (s_ap_active) close_ap();

    if (now - s_last_rssi_us > (int64_t)RSSI_REFRESH_MS * 1000) {
      s_last_rssi_us = now;
      wifi_ap_record_t record;
      if (esp_wifi_sta_get_ap_info(&record) == ESP_OK) s_rssi = record.rssi;
    }
    return;
  }

  if (s_sta_netif == NULL) return;  // no network configured: AP mode only

  if (s_down_since_us == 0) s_down_since_us = now;

  // Raise the setup AP once the configured network looks unreachable, but
  // keep trying: a router that boots late must not need a board restart.
  if (!s_ap_active && now - s_down_since_us > APP_WIFI_JOIN_TIMEOUT_US) {
    ESP_LOGW(TAG, "%d s without Wi-Fi: opening the setup AP as a fallback",
             CONFIG_APP_WIFI_JOIN_TIMEOUT_S);
    start_ap();
  }

  // Only ask for a new connection while the station is down: calling
  // esp_wifi_connect() while it is already associated tears down the DHCP
  // handshake that is still in flight.
  if (!s_sta_up && now - s_last_retry_us > (int64_t)JOIN_RETRY_MS * 1000) {
    s_last_retry_us = now;
    esp_wifi_connect();
  }
}

bool wifi_link_is_connected(void) { return s_connected; }

const char *wifi_link_ip(void) { return s_ip; }

int wifi_link_rssi(void) { return s_rssi; }

bool wifi_link_ap_active(void) { return s_ap_active; }

const char *wifi_link_state_text(void) {
  switch (s_state) {
    case LINK_WAITING:
      return s_ap_active ? "setup AP" : "no network";
    case LINK_CONNECTING:
      return s_ap_active ? "joining (setup AP up)" : "joining";
    case LINK_CONNECTED:
      return s_ap_active ? "connected (setup AP up)" : "connected";
    case LINK_OFF:
    default:
      return "off";
  }
}
