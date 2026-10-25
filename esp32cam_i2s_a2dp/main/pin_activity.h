// Boot time wiring check for the I2S input pins.
//
// The receiver runs as an I2S slave: when the ESP32-S3 is not clocking the bus
// every read just times out, which looks exactly like a pin or clock
// configuration mistake in the status log. Sampling the three lines as plain
// GPIOs before the I2S driver takes them over tells the two cases apart.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// BCLK, WS and DATA; one spare slot.
#define PIN_ACTIVITY_MAX 4

// Smallest number of level changes treated as "toggling": far above what a
// floating input picks up, far below a real I2S clock (hundreds of kHz or more).
#define PIN_ACTIVITY_ACTIVE_EDGES 100

typedef struct {
  int gpio;
  uint32_t transitions;  // level changes counted during the window
  uint32_t window_ms;    // sampling window, for reading the number in context
  int last_level;        // 1 = high, 0 = low, at the end of the window
} pin_activity_t;

// Reads every GPIO in gpios[] in a tight loop for window_ms and counts level
// changes. Pins are configured as inputs without pull resistors - the same
// thing the I2S driver wants - so the receiver can take them over afterwards.
// Returns false if the pins could not be configured.
bool pin_activity_measure(const int *gpios, size_t count, uint32_t window_ms,
                          pin_activity_t *activity);

// True when the line changed level often enough to be a driven signal.
bool pin_activity_is_toggling(const pin_activity_t *activity);

#ifdef __cplusplus
}
#endif
