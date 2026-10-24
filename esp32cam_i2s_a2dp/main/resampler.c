#include "resampler.h"

#include <math.h>
#include <string.h>

#include "app_config.h"

void resampler_init(resampler_t *resampler, int in_rate, int out_rate) {
  memset(resampler, 0, sizeof(*resampler));
  resampler->in_rate = in_rate;
  resampler->out_rate = out_rate;
  resampler->step = (double)in_rate / (double)out_rate;
}

size_t resampler_process(resampler_t *resampler, const int16_t *in,
                         size_t in_frames, int16_t *out,
                         size_t out_capacity) {
  if (in_frames == 0 || out_capacity == 0) return 0;

  if (resampler->in_rate == resampler->out_rate) {
    size_t count = in_frames < out_capacity ? in_frames : out_capacity;
    memcpy(out, in, count * APP_BYTES_PER_FRAME);
    return count;
  }

  size_t produced = 0;
  double position = resampler->position;

  // Positions up to (in_frames - 2) can be interpolated: the upper sample of
  // the pair is still inside this chunk. Anything later is carried over to
  // the next call via prev1/prev2.
  while (produced < out_capacity && position <= (double)(in_frames - 2)) {
    long index = (long)floor(position);
    double fraction = position - (double)index;

    const int16_t *lower;
    const int16_t *upper;
    long upper_index = index + 1;

    if (index >= 0) {
      lower = in + index * APP_AUDIO_CHANNELS;
    } else {
      lower = (index == -1) ? resampler->prev1 : resampler->prev2;
    }
    if (upper_index >= 0) {
      upper = in + upper_index * APP_AUDIO_CHANNELS;
    } else {
      upper = (upper_index == -1) ? resampler->prev1 : resampler->prev2;
    }

    for (int channel = 0; channel < APP_AUDIO_CHANNELS; channel++) {
      double value = (double)lower[channel] * (1.0 - fraction) +
                     (double)upper[channel] * fraction;
      long rounded = (long)(value + (value >= 0.0 ? 0.5 : -0.5));
      if (rounded > 32767) rounded = 32767;
      if (rounded < -32768) rounded = -32768;
      out[produced * APP_AUDIO_CHANNELS + channel] = (int16_t)rounded;
    }

    produced++;
    position += resampler->step;
  }

  if (in_frames >= 2) {
    resampler->prev2[0] = in[(in_frames - 2) * APP_AUDIO_CHANNELS];
    resampler->prev2[1] = in[(in_frames - 2) * APP_AUDIO_CHANNELS + 1];
  }
  resampler->prev1[0] = in[(in_frames - 1) * APP_AUDIO_CHANNELS];
  resampler->prev1[1] = in[(in_frames - 1) * APP_AUDIO_CHANNELS + 1];

  // Re-base the position onto the next chunk (its index -1 is our prev1).
  resampler->position = position - (double)in_frames;
  return produced;
}
