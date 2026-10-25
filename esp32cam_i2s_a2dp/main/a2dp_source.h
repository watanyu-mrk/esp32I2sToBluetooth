// A2DP source side: owns the BluetoothA2DPSource instance and feeds it from
// the audio buffer. Callable from C.
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Starts Bluetooth and connects to the sink configured in app_settings.
// Local name and SSP mode are taken from the same settings, so call
// app_settings_load() first.
void a2dp_source_start(void);

// True while a speaker is connected and the audio stream is started.
bool a2dp_source_is_streaming(void);

// True once a speaker accepted the A2DP connection (even before audio starts).
bool a2dp_source_is_connected(void);

// True as soon as a connection attempt starts, i.e. not DISCONNECTED. The
// config portal must be gone by then: the Bluetooth stack needs the heap it
// holds before it allocates the streaming buffers.
bool a2dp_source_is_active(void);

// True while device discovery is running.
bool a2dp_source_is_scanning(void);

// Human readable connection state for the periodic status log.
const char *a2dp_source_state(void);

#ifdef __cplusplus
}
#endif
