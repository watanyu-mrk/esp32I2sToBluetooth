// AP mode configuration portal: raises a Wi-Fi access point with a small web
// page for the Bluetooth settings. It is only meant to run after the bridge has
// given up on finding a speaker and shut Bluetooth down, so the Wi-Fi stack
// gets the radio and the heap for itself.
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Brings up the access point and the HTTP server. Returns false if either step
// fails; calling it while already running is a no-op that returns true.
bool config_portal_start(void);

// Shuts the HTTP server and the whole Wi-Fi stack down again, releasing the
// RAM it holds. Safe to call when the portal is not running.
void config_portal_stop(void);

bool config_portal_is_running(void);

#ifdef __cplusplus
}
#endif
