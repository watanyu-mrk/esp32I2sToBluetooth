// Linear interpolation resampler for interleaved 16-bit stereo PCM.
//
// Only used when the I2S input rate differs from the A2DP sample rate. It
// keeps two frames of history so the interpolation window stays valid across
// chunk boundaries, and it is deterministic: the same input produces the same
// output regardless of how the stream is split into chunks.
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  int in_rate;
  int out_rate;
  double step;      // input frames consumed per output frame
  double position;  // next sample position, relative to prev1 (index -1)
  int16_t prev2[2];
  int16_t prev1[2];
} resampler_t;

void resampler_init(resampler_t *resampler, int in_rate, int out_rate);

// Resamples in_frames stereo frames into out, producing at most out_capacity
// frames. Returns the number of frames written. Safe to call repeatedly with
// consecutive chunks of the same stream.
size_t resampler_process(resampler_t *resampler, const int16_t *in,
                         size_t in_frames, int16_t *out,
                         size_t out_capacity);

#ifdef __cplusplus
}
#endif
