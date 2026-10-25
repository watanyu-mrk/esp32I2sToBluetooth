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
// config portal is never started while this is true: the Bluetooth stack needs
// the radio and the heap before it allocates the streaming buffers.
bool a2dp_source_is_active(void);

// True while device discovery is running.
bool a2dp_source_is_scanning(void);

// Shuts Bluetooth down completely and releases the Classic BT memory, so the
// Wi-Fi stack can take the radio and the heap. There is no way back in this
// boot: Bluetooth cannot be initialised again, a restart is required (which is
// what saving the config page does). Safe to call when not started.
void a2dp_source_stop(void);

// True once a2dp_source_stop() released the Bluetooth stack.
bool a2dp_source_is_shut_down(void);

// Human readable connection state for the periodic status log.
const char *a2dp_source_state(void);

#ifdef __cplusplus
}
#endif
