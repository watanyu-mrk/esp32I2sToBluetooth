// Single producer / single consumer ring buffer holding interleaved 16-bit
// stereo PCM (one "frame" = one left + one right sample).
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  size_t capacity_frames;
  size_t available_frames;
  uint32_t dropped_frames;    // producer overflow: incoming chunk discarded
  uint32_t underflow_frames;  // consumer ran short and padded with silence
} audio_buffer_stats_t;

// capacity_bytes is rounded up to a power of two number of frames.
bool audio_buffer_init(size_t capacity_bytes);
void audio_buffer_deinit(void);

// Writes whole frames; returns the number of frames written (0 on overflow).
size_t audio_buffer_write(const int16_t *frames, size_t count);
// Reads whole frames; returns the number of frames actually read.
size_t audio_buffer_read(int16_t *frames, size_t count);

size_t audio_buffer_available_frames(void);
size_t audio_buffer_free_frames(void);
// Drops everything currently buffered (used while nothing is streaming).
void audio_buffer_reset(void);

void audio_buffer_record_underflow(size_t frames);
audio_buffer_stats_t audio_buffer_get_stats(void);

#ifdef __cplusplus
}
#endif
