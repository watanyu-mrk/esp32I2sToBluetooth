// Internet radio stream: opens the current station's URL, decodes MP3 and
// pushes PCM16 stereo into the audio buffer that the A2DP source drains.
//
// The task only runs while a Bluetooth speaker is streaming; when the speaker
// goes away the socket is closed too (a paused reader would only pile up stale
// audio, and live radio is not worth buffering ahead of the speaker).
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "app_settings.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  bool enabled;       // a speaker is streaming, playback is wanted
  bool connected;     // the HTTP stream is open
  char state[24];     // human readable state for the page/status log
  char content_type[32];
  char station_name[APP_SETTINGS_NAME_LEN];
  uint8_t station_index;  // 0-based index in app_settings_t::stations
  uint8_t station_count;
  uint32_t sample_rate;   // as reported by the last decoded frame
  uint32_t bitrate;       // kbps, as reported by the last decoded frame
  uint8_t channels;       // 1 or 2, as reported by the last decoded frame
  uint32_t frames;        // PCM frames handed to the audio buffer
  uint32_t dropped_frames;// PCM frames discarded because the buffer was full
  uint32_t decode_errors; // damaged frames skipped
  uint32_t connects;      // successful HTTP connections
  uint32_t failures;      // failed connection attempts
  uint32_t bytes;         // stream bytes read from the socket
} radio_stream_info_t;

// Creates the stream task. Call once at boot; the task idles until
// radio_stream_set_enabled(true).
void radio_stream_start(void);

// Asks the task to open/close the stream. Cheap to call repeatedly.
void radio_stream_set_enabled(bool enabled);

// Pause/resume from the speaker's buttons (AVRCP). A paused radio closes the
// socket, so resuming tunes in to the live stream instead of replaying what
// was buffered while nobody was listening.
void radio_stream_set_paused(bool paused);
bool radio_stream_is_paused(void);

// Drops the current connection and tunes in again at the live edge.
void radio_stream_restart(void);

// Next/previous station (AVRCP forward/backward). With a single station this
// just retunes to the live edge.
void radio_stream_next_station(void);
void radio_stream_previous_station(void);

// True while the stream socket is open.
bool radio_stream_is_running(void);

// Snapshot for the status log and the config page.
void radio_stream_get_info(radio_stream_info_t *out);

#ifdef __cplusplus
}
#endif
