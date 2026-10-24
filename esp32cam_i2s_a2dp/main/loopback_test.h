// Hardware self test of the I2S receive path, no ESP32-S3 and no Bluetooth
// required: I2S1 (master TX) drives BCLK/WS/DATA on exactly the pins the
// receiver listens on, so the real receiver task, 32-bit slot -> PCM16
// conversion, stereo order and audio buffer are all exercised.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Requires i2s_receiver_start() to have been called with a gate that lets
// data through (NULL is fine). Drives the transmitter, waits, then verifies
// what landed in the audio buffer. Logs a PASS/FAIL line and returns it.
bool loopback_test_run(int bclk_gpio, int ws_gpio, int data_gpio,
                       int shift_bits, uint32_t sample_rate);

#ifdef __cplusplus
}
#endif
