// A2DP source side: owns the BluetoothA2DPSource instance and feeds it from
// the audio buffer. Callable from C.
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Starts Bluetooth, discovers/connects to target_name and begins streaming.
// An empty target_name connects to the first A2DP sink that is discovered.
void a2dp_source_start(const char *target_name);

// True while a speaker is connected and the audio stream is started.
bool a2dp_source_is_streaming(void);

// Human readable connection state for the periodic status log.
const char *a2dp_source_state(void);

#ifdef __cplusplus
}
#endif
