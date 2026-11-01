// Wi-Fi link: joins the configured network and, when that is impossible,
// raises a setup access point so the config page is still reachable.
//
// The board is both a client (it downloads the stream) and, in the failure
// case, an access point. Station and AP can run together (WIFI_MODE_APSTA);
// the AP only comes up when there is no other way in.
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Initialises the TCP/IP stack and the Wi-Fi driver, then starts the station
// (or the setup access point when no SSID is configured). Safe to call once.
bool wifi_link_start(void);

// Must be called from the main loop: retries the station, raises the setup AP
// after APP_WIFI_JOIN_TIMEOUT_MS without a connection, and refreshes the RSSI.
void wifi_link_poll(void);

bool wifi_link_is_connected(void);

// Dotted IPv4 address of the station interface, or an empty string.
const char *wifi_link_ip(void);

// RSSI of the connected network in dBm, 0 when not connected.
int wifi_link_rssi(void);

bool wifi_link_ap_active(void);

// Human readable state for the status log and the config page.
const char *wifi_link_state_text(void);

#ifdef __cplusplus
}
#endif
