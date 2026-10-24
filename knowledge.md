# ESP32-A2DP — Project Knowledge

## What this is

Arduino/ESP-IDF C++ library (v1.8.11, Apache-2.0) that wraps the ESP-IDF Bluedroid
A2DP stack so an ESP32 can be a **Bluetooth audio sink (receiver)** or **source
(sender)**, with AVRCP. HFP/HSP are not supported. Only the classic-Bluetooth
original ESP32 (and `CONFIG_IDF_TARGET_ESP32S31`) can run it — see
`src/config.h` (`IS_VALID_PLATFORM`), which silences the whole API elsewhere.

The library lives in `ESP32-A2DP-main/`; the repository root also holds
`promp.txt`, a Thai-language spec for a separate ESP32-S3 → ESP32-CAM I2S/A2DP
bridge that consumes this library's **Source** side. That bridge is implemented
in `esp32cam_i2s_a2dp/` (ESP-IDF 5.5 project, target `esp32`, plus a local run of
the I2S loopback and resampler self-tests via `CONFIG_APP_LOOPBACK_TEST`); see its
`README.md` for wiring, build and hardware test results, and `STATUS.md` for the
handoff note (what is verified on hardware, what is still untested, next steps).

## Where the code is

| Path | Role |
|---|---|
| `src/config.h` | All feature flags, version gates and tunables — read this first |
| `src/BluetoothA2DP.h` | Umbrella header (Sink + SinkQueued + Source) |
| `src/BluetoothA2DPCommon.{h,cpp}` | Shared base: `start()/end()/stop()`, `set_auto_reconnect()`, `set_volume()`, `is_connected()` |
| `src/BluetoothA2DPSink.{h,cpp}` | Sink: event handling, AVRCP, metadata, `add_decoder()`, output wiring |
| `src/BluetoothA2DPSinkQueued.{h,cpp}` | Sink writing from a FreeRTOS task through a ringbuffer (`A2DP_I2S_MAX_WRITE_SIZE`) |
| `src/BluetoothA2DPSource.{h,cpp}` | Source: `set_data_callback()`, `set_data_callback_in_frames()`, `start_raw()`, `add_encoder()` |
| `src/BluetoothA2DPOutput.{h,cpp}` | Output backends: legacy I2S vs AudioTools/`Print`, plus default pins |
| `src/A2DPDecoder*.h/.cpp` | Managed multi-codec decode (`A2DPDecoderSBC`, `A2DPDecoderAAC`) |
| `src/A2DPEncoder*.h/.cpp` | Managed multi-codec encode (`A2DPEncoderSBC`, `A2DPEncoderAAC`) |
| `src/A2DPVolumeControl.h` | Volume curves/strategies (`set_volume_control()`) |
| `examples/` | `.ino` sketches: `bt_music_sender*`, plus legacy sink sketches under `examples/legacy-api/` |
| `Doxyfile`, `docs/html` | Class docs (`doxygen Doxyfile` from `ESP32-A2DP-main/`); output is checked in |

There is **no test suite, no CI workflow, no platformio.ini/package.json** — a
change is "verified" only by compiling a sketch/IDF project and, where
relevant, running on hardware.

## Build / install

Arduino: copy or clone into `~/Documents/Arduino/libraries` (ideally as
`ESP32-A2DP`), then `arduino-cli compile --fqbn esp32:esp32:esp32 <sketch>`
or use the IDE. **Most examples also need `arduino-audio-tools`**
(`AudioTools.h`) cloned alongside.

ESP-IDF component: `CMakeLists.txt` calls `idf_component_register(SRC_DIRS src
INCLUDE_DIRS src REQUIRES bt esp_common freertos hal log nvs_flash driver
esp_ringbuf arduino-audio-tools)` and force-enables
`-DA2DP_LEGACY_I2S_SUPPORT=0 -DA2DP_I2S_AUDIOTOOLS=1`. Put the folder in
`components/` or pull it via `idf_component.yml`, then `idf.py build`. On
IDF > 6, codec components (`codec-helix`, `codec-sbc`, `codec-faad`,
`codec-fdk-aac`) are appended to `REQUIRES` only if present in
`COMPONENT_DIRS` — don't add them unconditionally.

## Version / feature gates (all in `src/config.h`)

- `A2DP_I2S_AUDIOTOOLS` — auto-enabled when `AudioTools.h` is visible
  (`__has_include`); otherwise the sink emits `#warning "AudioTools library is
  not included first or installed"`.
- `A2DP_LEGACY_I2S_SUPPORT` — defaults to `IDF < 5.0`; the legacy I2S API is
  gone with Arduino core 3.x / IDF 5.
- `A2DP_MANAGED_DECODER_SUPPORTED` — needs IDF ≥ 5.5 + AudioTools + valid platform.
- `A2DP_MANAGED_ENCODER_SUPPORTED` — needs IDF ≥ 5.5 + AudioTools, and at
  runtime `CONFIG_BT_A2DP_USE_EXTERNAL_CODEC=y`.
- Registering an **AAC** stream endpoint needs IDF ≥ 6.1,
  `CONFIG_BT_A2DP_CODEC_AAC_ENABLED=y` and `CONFIG_BT_A2DP_SEP_NUM_MAX` ≥
  number of `add_decoder()`/`add_encoder()` calls. Failure is reported at
  runtime (`ESP_A2D_SEP_REG_STATE_EVT`), not at compile time.
- `A2DP_SPP_SUPPORT` (default 1) pulls in `esp_spp_api.h` on IDF ≥ 5.
- Pacing/tuning knobs: `A2DP_MANAGED_ENCODE_TASK_STACK` (raise to ~40000 for
  AAC), `A2DP_MANAGED_ENCODE_CHUNK_SIZE/_LEAD_MS/_MAX_LAG_MS/_SEND_RETRY_MS`,
  `A2DP_MANAGED_DECODE_Q_DEPTH/_TASK_STACK/_TASK_PRIO`,
  `A2DP_I2S_MAX_WRITE_SIZE`, `A2DP_DISCONNECT_LIMIT`, `AUTOCONNECT_TRY_NUM`.

## Conventions

- 2-space indent, `#pragma once`, snake_case methods (`set_sample_rate`,
  `add_encoder`), `p_`/`p_print` prefix for pointers, `ccall_*` `extern "C"`
  trampolines + `friend` declarations to bridge C callbacks into classes.
- Every public class/member gets a Doxygen `///` or `/** */` comment, often
  with `@ingroup a2dp` / `@ingroup codecs` and `@copyright Apache License
  Version 2`.
- Logging via `ESP_LOGI/W/E(<TAG>, ...)` with per-file tags (`"A2DPEncoder"`,
  `BT_AV_TAG`, `BT_APP_TAG`); no `printf`, no `Serial.print` in `src/`.
- Codec support is compositional: subclass `A2DPEncoder`/`A2DPDecoder`,
  implement `codec_type()`, `mime()`, `build_capability()`,
  `samples_per_frame()`, `parse_audio_info()`, then register the instance —
  never special-case a codec in the sink/source.

## Gotchas

- **`add_decoder()` replaces the built-in SBC decode path entirely.** If you
  register any decoder you must also register `A2DPDecoderSBC`, otherwise SBC
  sources go silent (the note in `CMakeLists.txt` says exactly this).
- PCM across the library is always **16-bit stereo** at the negotiated/declared
  sample rate (source default 44100). Sink output is forced to 16-bit stereo in
  `BluetoothA2DPOutputAudioTools::set_sample_rate()`; changing sample rate does
  not change slot width.
- Default output pins differ by backend: legacy I2S defaults to BCK 26 / WS 25
  / DATA 22 (`BluetoothA2DPOutput.cpp`), while the README example quotes
  14/15/22 for the AudioTools `I2SStream` path. Always set pins explicitly.
- `start()` may be called once per instance; re-`start()` after `end()` needs
  `end(true)` to release memory, and repeated stack-init/event handling is the
  usual cause of crashes — use the auto-reconnect helpers instead.
- Sink examples other than the sender set have moved to `examples/legacy-api/`;
  `CMakeLists.txt` still mentions `examples/bt_music_receiver_codec`, which is
  not in this snapshot.
- No formatting config in-repo; match the surrounding file rather than
  reformatting, and keep `docs/html` regenerations out of unrelated changes.
