// Config portal: the HTTP server that serves the settings page. It runs the
// whole time (not only in a "config mode"), so the station URL and the
// Bluetooth settings can be changed while the radio is playing.
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Starts the HTTP server. Returns false if it cannot be started; calling it
// while already running is a no-op that returns true.
bool config_portal_start(void);

bool config_portal_is_running(void);

#ifdef __cplusplus
}
#endif
