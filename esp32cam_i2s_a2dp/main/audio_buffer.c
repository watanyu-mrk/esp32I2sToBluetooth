#include "audio_buffer.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "app_config.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "audio_buf";

// Capacity is a power of two so the indices can wrap with a mask. head/tail
// are free running frame counters; the difference is the number of frames
// currently buffered.
static int16_t *s_samples;
static size_t s_capacity_frames;
static size_t s_mask;

static atomic_size_t s_head;
static atomic_size_t s_tail;
static atomic_uint_fast32_t s_dropped_frames;
static atomic_uint_fast32_t s_underflow_frames;

static size_t next_power_of_two(size_t value) {
  size_t result = 1;
  while (result < value) result <<= 1;
  return result;
}

bool audio_buffer_init(size_t capacity_bytes) {
  if (s_samples != NULL) return true;

  size_t frames = capacity_bytes / APP_BYTES_PER_FRAME;
  if (frames < 256) frames = 256;
  size_t requested_frames = next_power_of_two(frames);
  frames = requested_frames;

  // Internal RAM: the buffer is written by an ISR-fed DMA task and read from
  // the Bluetooth task, so it must not live in (slower) PSRAM.
  //
  // It has to be one contiguous block, and internal RAM is split into regions
  // of ~110 KB, so a large buffer_ms setting can ask for more than any single
  // region holds. Halving the size until the allocation succeeds degrades the
  // jitter buffer instead of stopping the bridge from booting at all - the
  // config page is not reachable otherwise.
  while (true) {
    s_samples = heap_caps_malloc(frames * APP_BYTES_PER_FRAME,
                                 MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (s_samples != NULL) break;
    if (frames <= 256) {
      ESP_LOGE(TAG, "cannot allocate %u bytes for the audio buffer",
               (unsigned)(frames * APP_BYTES_PER_FRAME));
      return false;
    }
    frames >>= 1;
  }
  if (frames != requested_frames) {
    ESP_LOGW(TAG,
             "only %u bytes could be allocated contiguously: buffer reduced "
             "to %u ms (asked for %u ms)",
             (unsigned)(frames * APP_BYTES_PER_FRAME),
             (unsigned)(frames * 1000 / APP_OUTPUT_SAMPLE_RATE),
             (unsigned)(requested_frames * 1000 / APP_OUTPUT_SAMPLE_RATE));
  }

  s_capacity_frames = frames;
  s_mask = frames - 1;
  atomic_store(&s_head, 0);
  atomic_store(&s_tail, 0);
  atomic_store(&s_dropped_frames, 0);
  atomic_store(&s_underflow_frames, 0);

  ESP_LOGI(TAG, "buffer: %u frames (%u ms) in internal RAM",
           (unsigned)s_capacity_frames,
           (unsigned)(s_capacity_frames * 1000 / APP_OUTPUT_SAMPLE_RATE));
  return true;
}

void audio_buffer_deinit(void) {
  free(s_samples);
  s_samples = NULL;
  s_capacity_frames = 0;
  s_mask = 0;
}

size_t audio_buffer_write(const int16_t *frames, size_t count) {
  if (s_samples == NULL || count == 0) return 0;

  size_t head = atomic_load_explicit(&s_head, memory_order_relaxed);
  size_t tail = atomic_load_explicit(&s_tail, memory_order_acquire);
  size_t used = head - tail;

  if (count > s_capacity_frames - used) {
    // Drop the whole chunk: a partial write would tear a stereo frame.
    atomic_fetch_add(&s_dropped_frames, (uint_fast32_t)count);
    return 0;
  }

  size_t start = head & s_mask;
  size_t first = s_capacity_frames - start;
  if (first > count) first = count;

  memcpy(&s_samples[start * APP_AUDIO_CHANNELS], frames,
         first * APP_BYTES_PER_FRAME);
  if (count > first) {
    memcpy(&s_samples[0], frames + first * APP_AUDIO_CHANNELS,
           (count - first) * APP_BYTES_PER_FRAME);
  }

  atomic_store_explicit(&s_head, head + count, memory_order_release);
  return count;
}

size_t audio_buffer_read(int16_t *frames, size_t count) {
  if (s_samples == NULL || count == 0) return 0;

  size_t tail = atomic_load_explicit(&s_tail, memory_order_relaxed);
  size_t head = atomic_load_explicit(&s_head, memory_order_acquire);
  size_t available = head - tail;
  if (count > available) count = available;
  if (count == 0) return 0;

  size_t start = tail & s_mask;
  size_t first = s_capacity_frames - start;
  if (first > count) first = count;

  memcpy(frames, &s_samples[start * APP_AUDIO_CHANNELS],
         first * APP_BYTES_PER_FRAME);
  if (count > first) {
    memcpy(frames + first * APP_AUDIO_CHANNELS, &s_samples[0],
           (count - first) * APP_BYTES_PER_FRAME);
  }

  atomic_store_explicit(&s_tail, tail + count, memory_order_release);
  return count;
}

size_t audio_buffer_available_frames(void) {
  if (s_samples == NULL) return 0;
  size_t head = atomic_load_explicit(&s_head, memory_order_acquire);
  size_t tail = atomic_load_explicit(&s_tail, memory_order_acquire);
  return head - tail;
}

size_t audio_buffer_free_frames(void) {
  if (s_samples == NULL) return 0;
  return s_capacity_frames - audio_buffer_available_frames();
}

void audio_buffer_reset(void) {
  if (s_samples == NULL) return;
  atomic_store(&s_tail, atomic_load(&s_head));
}

void audio_buffer_record_underflow(size_t frames) {
  atomic_fetch_add(&s_underflow_frames, (uint_fast32_t)frames);
}

audio_buffer_stats_t audio_buffer_get_stats(void) {
  audio_buffer_stats_t stats = {0};
  stats.capacity_frames = s_capacity_frames;
  stats.available_frames = audio_buffer_available_frames();
  stats.dropped_frames = (uint32_t)atomic_load(&s_dropped_frames);
  stats.underflow_frames = (uint32_t)atomic_load(&s_underflow_frames);
  return stats;
}
