#include "radio_stream.h"

#include <inttypes.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mp3dec.h"

#include "app_config.h"
#include "app_settings.h"
#include "audio_buffer.h"
#include "resampler.h"

static const char *TAG = "radio";

// How long the socket reader waits for the jitter buffer to make room. This
// is the stream's backpressure: while the buffer is full the read simply
// stops, TCP holds the audio and nothing is lost. The cap only matters when
// the speaker stops pulling for good, where dropping beats blocking forever.
#define QUEUE_WAIT_MS 10
#define QUEUE_MAX_WAIT_MS 5000

static TaskHandle_t s_task;
static volatile bool s_enabled;
// Paused from the remote control (AVRCP): the socket is closed and resuming
// reconnects, so a pause never leaves a growing backlog of stale audio.
static volatile bool s_paused;
// Tune-in-again request (remote next/previous): break out of the read loop.
static volatile bool s_restart;
// Which station of app_settings_t::stations is playing, 0-based.
static volatile uint8_t s_station_index;

static HMP3Decoder s_decoder;
static int s_stream_rate;  // rate of the decoder output, for the resampler
static resampler_t s_resampler;

// Decode scratch buffers, in PSRAM on a WROVER (internal RAM is what the
// Bluetooth and Wi-Fi stacks need) with an internal fallback.
static int16_t *s_pcm;      // raw decoder output, interleaved
static int16_t *s_stereo;   // decoder output forced to stereo, stream rate
static int16_t *s_resampled;  // stereo, 44100 Hz

// MP3 bytes waiting to be decoded. Static so it is not charged to the task
// stack; the decoder needs a whole frame at a time.
static unsigned char s_in[APP_RADIO_INPUT_BYTES];

static struct {
  volatile bool connected;
  volatile uint32_t sample_rate;
  volatile uint32_t bitrate;
  volatile uint32_t channels;
  volatile uint32_t frames;
  volatile uint32_t dropped_frames;
  volatile uint32_t decode_errors;
  volatile uint32_t connects;
  volatile uint32_t failures;
  volatile uint32_t bytes;
  volatile int http_status;
  volatile uint32_t station_index;
  char station_name[APP_SETTINGS_NAME_LEN];
  char content_type[32];
} s_info;

static int16_t *alloc_scratch(void) {
  size_t bytes = (size_t)APP_PCM_FRAMES * APP_AUDIO_CHANNELS * sizeof(int16_t);
  int16_t *memory = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (memory == NULL) {
    memory = heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  }
  return memory;
}

// ---------------------------------------------------------------------------
// Feeding the Bluetooth side
// ---------------------------------------------------------------------------

// Waits until the whole chunk fits in the jitter buffer. Not reading the
// socket is the correct backpressure for a live stream: the server's data
// stays in the TCP window, so the buffer simply stays full and playback never
// jumps. Dropping whole chunks here (the previous behaviour) is audible as
// short gaps right after every burst.
static void queue_frames(const int16_t *frames, size_t count) {
  size_t written = 0;
  int waited_ms = 0;

  while (written < count) {
    size_t accepted =
        audio_buffer_write(frames + written * APP_AUDIO_CHANNELS,
                           count - written);
    if (accepted > 0) {
      written += accepted;
      continue;
    }

    if (!s_enabled || s_paused) return;  // stopping: drop the rest
    if (waited_ms >= QUEUE_MAX_WAIT_MS) {
      audio_buffer_record_dropped(count - written);
      s_info.dropped_frames += (uint32_t)(count - written);
      return;
    }
    vTaskDelay(pdMS_TO_TICKS(QUEUE_WAIT_MS));
    waited_ms += QUEUE_WAIT_MS;
  }

  s_info.frames += (uint32_t)count;
}

// Converts one decoded frame to the 44100 Hz stereo stream the A2DP encoder
// expects and queues it.
static void emit_frame(const MP3FrameInfo *frame) {
  int channels = frame->nChans == 1 ? 1 : 2;
  if (frame->outputSamps <= 0 || frame->samprate <= 0) return;

  int samples = frame->outputSamps / channels;  // per channel
  if (samples <= 0 || samples > APP_PCM_FRAMES) {
    s_info.decode_errors++;
    return;
  }

  if (channels == 2) {
    memcpy(s_stereo, s_pcm, (size_t)samples * APP_BYTES_PER_FRAME);
  } else {
    for (int i = 0; i < samples; i++) {
      s_stereo[i * 2] = s_pcm[i];
      s_stereo[i * 2 + 1] = s_pcm[i];
    }
  }

  const int16_t *out = s_stereo;
  size_t out_frames = (size_t)samples;

  if (frame->samprate != APP_OUTPUT_SAMPLE_RATE && samples >= 2) {
    // Stations are commonly 44.1 kHz, but 48 kHz (and the odd 32/22.05 kHz)
    // ones exist and the SBC encoder is fixed at 44100 Hz.
    if (s_stream_rate != frame->samprate) {
      resampler_init(&s_resampler, frame->samprate, APP_OUTPUT_SAMPLE_RATE);
      s_stream_rate = frame->samprate;
      ESP_LOGI(TAG, "resampling %d Hz -> %d Hz", frame->samprate,
               APP_OUTPUT_SAMPLE_RATE);
    }
    out_frames = resampler_process(&s_resampler, s_stereo, (size_t)samples,
                                  s_resampled, APP_PCM_FRAMES);
    out = s_resampled;
  } else if (s_stream_rate != frame->samprate) {
    s_stream_rate = frame->samprate;
  }

  queue_frames(out, out_frames);

  s_info.sample_rate = (uint32_t)frame->samprate;
  s_info.bitrate = (uint32_t)(frame->bitrate / 1000);
  s_info.channels = (uint32_t)channels;
}

// ---------------------------------------------------------------------------
// MP3 parsing
// ---------------------------------------------------------------------------

// Decodes as many complete frames as the buffer holds and returns the number
// of bytes that are still undecoded (they stay at the start of buf).
static size_t decode_available(unsigned char *buf, size_t len) {
  while (len >= 4) {
    int sync = MP3FindSyncWord(buf, (int)len);
    if (sync < 0) {
      // No sync word: keep the last byte, the next frame may start there.
      buf[0] = buf[len - 1];
      return 1;
    }
    if (sync > 0) {
      memmove(buf, buf + sync, len - (size_t)sync);
      len -= (size_t)sync;
    }

    // Never call the decoder with a partial frame: it would report an
    // underflow and lose the frame header it had already consumed.
    if (len < APP_MP3_MAX_FRAME_BYTES) return len;

    unsigned char *frame = buf;
    int left = (int)len;
    int err = MP3Decode(s_decoder, &frame, &left, s_pcm, 0);

    if (err != 0) {
      // Joining a live stream happens mid-frame (bit reservoir missing) and
      // a damaged frame is possible on a lossy link: drop the sync byte and
      // look for the next frame either way.
      s_info.decode_errors++;
      memmove(buf, buf + 1, len - 1);
      len -= 1;
      continue;
    }

    MP3FrameInfo info;
    MP3GetLastFrameInfo(s_decoder, &info);
    emit_frame(&info);

    size_t consumed = len - (size_t)left;
    if (consumed == 0 || consumed > len) consumed = 1;
    memmove(buf, buf + consumed, len - consumed);
    len -= consumed;
  }
  return len;
}

// ---------------------------------------------------------------------------
// HTTP stream
// ---------------------------------------------------------------------------

static void log_icy_header(esp_http_client_handle_t client, const char *name) {
  char *value = NULL;
  if (esp_http_client_get_header(client, name, &value) == ESP_OK &&
      value != NULL) {
    ESP_LOGI(TAG, "%s: %s", name, value);
  }
}

// One connection attempt plus the streaming loop. Returns when the stream
// ends, fails, or streaming is no longer wanted.
static bool stream_once(void) {
  const app_settings_t *settings = app_settings_get();

  // The station can change between connections (remote next/previous), so it
  // is resolved on every attempt instead of being cached at start.
  uint8_t index = s_station_index;
  if (index >= settings->station_count) index = 0;
  const app_station_t *station = &settings->stations[index];

  s_info.station_index = index;
  snprintf(s_info.station_name, sizeof(s_info.station_name), "%s",
           station->name);

  ESP_LOGI(TAG, "station %u/%u '%s'", (unsigned)(index + 1),
           (unsigned)settings->station_count,
           station->name[0] ? station->name : "(unnamed)");

  esp_http_client_config_t config = {
      .url = station->url,
      .crt_bundle_attach = esp_crt_bundle_attach,
      .timeout_ms = 10000,
      .buffer_size = 2048,
      .buffer_size_tx = 512,
      .keep_alive_enable = true,
      .user_agent = "ESP32-Radio/1.0",
  };

  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (client == NULL) {
    ESP_LOGE(TAG, "cannot create the HTTP client");
    return false;
  }

  ESP_LOGI(TAG, "connecting to %s", station->url);
  esp_err_t err = esp_http_client_open(client, 0);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "open failed: %s", esp_err_to_name(err));
    esp_http_client_cleanup(client);
    return false;
  }

  esp_http_client_fetch_headers(client);
  int status = esp_http_client_get_status_code(client);
  s_info.http_status = status;

  char *content_type = NULL;
  esp_http_client_get_header(client, "Content-Type", &content_type);
  snprintf(s_info.content_type, sizeof(s_info.content_type), "%s",
           content_type != NULL ? content_type : "");

  if (status != 200) {
    ESP_LOGE(TAG, "station answered HTTP %d", status);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return false;
  }

  ESP_LOGI(TAG, "stream open (Content-Type '%s')",
           s_info.content_type[0] ? s_info.content_type : "?");
  log_icy_header(client, "icy-name");
  log_icy_header(client, "icy-br");

  // A fresh connection has a fresh bit reservoir, and the log line above is
  // the only trace of which station is playing.
  if (s_decoder != NULL) MP3FreeDecoder(s_decoder);
  s_decoder = MP3InitDecoder();
  if (s_decoder == NULL) {
    ESP_LOGE(TAG, "cannot allocate the MP3 decoder");
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return false;
  }
  s_stream_rate = 0;

  s_info.connects++;
  s_info.connected = true;

  size_t pending = 0;
  bool ok = true;
  // Reads that return no data are normal: the socket is non-blocking, so a TLS
  // record that is still in flight shows up as read()==0 with errno EAGAIN
  // (surfaced here as ESP_FAIL). Only a long silence means the station is
  // really gone - treating the first empty read as end-of-stream dropped a
  // healthy connection every time the link hiccuped.
  const int idle_limit_ms = 10000;
  int idle_ms = 0;

  while (s_enabled && !s_paused) {
    if (s_restart) {
      ESP_LOGI(TAG, "remote control asked for the live edge: retuning");
      s_restart = false;
      break;
    }

    if (pending >= sizeof(s_in)) {
      // Should not happen (decode_available always makes progress), but never
      // spin on a full buffer.
      ESP_LOGW(TAG, "input buffer full, resynchronising");
      pending = 0;
    }

    int read = esp_http_client_read(client, (char *)(s_in + pending),
                                    (int)(sizeof(s_in) - pending));
    if (read > 0) {
      s_info.bytes += (uint32_t)read;
      idle_ms = 0;
      pending = decode_available(s_in, pending + (size_t)read);
      continue;
    }

    idle_ms += 20;
    if (idle_ms > idle_limit_ms) {
      ESP_LOGW(TAG, "no data from the station for %d ms, reconnecting",
               idle_ms);
      ok = false;
      break;
    }
    // Yield so the Wi-Fi and Bluetooth tasks can run: this loop is the only
    // consumer of the socket.
    vTaskDelay(pdMS_TO_TICKS(20));
  }

  s_info.connected = false;
  esp_http_client_close(client);
  esp_http_client_cleanup(client);
  return ok;
}

// ---------------------------------------------------------------------------
// Task
// ---------------------------------------------------------------------------

static void wait_retry(uint32_t delay_ms) {
  while (delay_ms > 0 && !s_enabled) {
    vTaskDelay(pdMS_TO_TICKS(100));
    delay_ms = delay_ms > 100 ? delay_ms - 100 : 0;
  }
  while (delay_ms > 0 && s_enabled) {
    uint32_t step = delay_ms > 100 ? 100 : delay_ms;
    vTaskDelay(pdMS_TO_TICKS(step));
    delay_ms -= step;
  }
}

static void radio_task(void *arg) {
  (void)arg;

  s_pcm = alloc_scratch();
  s_stereo = alloc_scratch();
  s_resampled = alloc_scratch();
  if (s_pcm == NULL || s_stereo == NULL || s_resampled == NULL) {
    ESP_LOGE(TAG, "cannot allocate the decoder buffers - radio disabled");
    vTaskDelete(NULL);
    return;
  }

  uint32_t backoff_ms = APP_RADIO_RETRY_MIN_MS;

  while (true) {
    if (!s_enabled || s_paused) {
      vTaskDelay(pdMS_TO_TICKS(200));
      continue;
    }

    bool ok = stream_once();

    if (s_decoder != NULL) {
      MP3FreeDecoder(s_decoder);
      s_decoder = NULL;
    }
    // Whatever was decoded but never played belongs to the previous
    // connection; keeping it would play stale audio after a reconnect.
    audio_buffer_reset();
    s_restart = false;

    // Paused or stopped on purpose is not a connection failure.
    if (!s_enabled || s_paused) {
      backoff_ms = APP_RADIO_RETRY_MIN_MS;
      continue;
    }

    if (ok) {
      backoff_ms = APP_RADIO_RETRY_MIN_MS;
      continue;  // stream ended cleanly, reconnect immediately
    }

    s_info.failures++;
    wait_retry(backoff_ms);
    backoff_ms *= 2;
    if (backoff_ms > APP_RADIO_RETRY_MAX_MS) backoff_ms = APP_RADIO_RETRY_MAX_MS;
  }
}

void radio_stream_start(void) {
  if (s_task != NULL) return;

  BaseType_t created = xTaskCreatePinnedToCore(
      radio_task, "radio", APP_RADIO_TASK_STACK, NULL, APP_RADIO_TASK_PRIO,
      &s_task, APP_RADIO_TASK_CORE);
  if (created != pdPASS) {
    ESP_LOGE(TAG, "cannot start the radio task");
    s_task = NULL;
  }
}

void radio_stream_set_enabled(bool enabled) {
  if (s_enabled == enabled) return;
  s_enabled = enabled;
  ESP_LOGI(TAG, "stream %s", enabled ? "enabled" : "stopped");
}

bool radio_stream_is_running(void) { return s_info.connected; }

void radio_stream_set_paused(bool paused) {
  if (s_paused == paused) return;
  s_paused = paused;
  ESP_LOGW(TAG, "%s from the remote control", paused ? "paused" : "resumed");
}

bool radio_stream_is_paused(void) { return s_paused; }

void radio_stream_restart(void) {
  s_restart = true;
  audio_buffer_reset();
  ESP_LOGW(TAG, "retune requested from the remote control");
}

static void switch_station(int delta) {
  const app_settings_t *settings = app_settings_get();
  uint8_t count = settings->station_count;

  if (count <= 1) {
    // Nothing to cycle through: next/previous just retunes the live edge.
    radio_stream_restart();
    return;
  }

  uint8_t index = (uint8_t)((s_station_index + count + delta) % count);
  s_station_index = index;
  s_restart = true;
  audio_buffer_reset();

  ESP_LOGW(TAG, "station %u/%u: %s", (unsigned)(index + 1), (unsigned)count,
           settings->stations[index].name[0] ? settings->stations[index].name
                                              : settings->stations[index].url);
}

void radio_stream_next_station(void) { switch_station(1); }

void radio_stream_previous_station(void) { switch_station(-1); }

void radio_stream_get_info(radio_stream_info_t *out) {
  if (out == NULL) return;

  const app_settings_t *settings = app_settings_get();

  memset(out, 0, sizeof(*out));
  out->enabled = s_enabled;
  out->connected = s_info.connected;
  out->station_index = (uint8_t)s_info.station_index;
  out->station_count = settings->station_count;
  memcpy(out->station_name, s_info.station_name, sizeof(out->station_name));
  out->sample_rate = s_info.sample_rate;
  out->bitrate = s_info.bitrate;
  out->channels = (uint8_t)s_info.channels;
  out->frames = s_info.frames;
  out->dropped_frames = s_info.dropped_frames;
  out->decode_errors = s_info.decode_errors;
  out->connects = s_info.connects;
  out->failures = s_info.failures;
  out->bytes = s_info.bytes;
  memcpy(out->content_type, s_info.content_type, sizeof(out->content_type));

  if (!s_enabled) {
    snprintf(out->state, sizeof(out->state), "idle (no speaker)");
  } else if (s_paused) {
    snprintf(out->state, sizeof(out->state), "paused (remote)");
  } else if (s_info.connected) {
    snprintf(out->state, sizeof(out->state), "playing");
  } else if (s_info.failures > 0) {
    snprintf(out->state, sizeof(out->state), "reconnecting");
  } else {
    snprintf(out->state, sizeof(out->state), "connecting");
  }
}
