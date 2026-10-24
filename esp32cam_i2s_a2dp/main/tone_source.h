// Generated 1 kHz test tone, used by CONFIG_APP_TEST_TONE to exercise the
// Bluetooth path without an I2S source connected.
#pragma once

#include <stdbool.h>

#include "app_config.h"

#ifdef __cplusplus
extern "C" {
#endif

bool tone_source_start(audio_gate_t gate);
void tone_source_stop(void);

#ifdef __cplusplus
}
#endif
