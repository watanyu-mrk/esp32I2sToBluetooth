# สถานะงาน (handoff) — กลับมาทำต่อจากไฟล์นี้

อัปเดตล่าสุด: 30 ต.ค. 2026 · บอร์ดทดสอบ: ESP32-CAM (ESP32-D0WD) ที่ **COM6**
(รอบล่าสุด: เพิ่ม **รหัส PIN สำหรับจับคู่แบบ Legacy** ไว้ตั้งได้จากหน้าเว็บ — สำหรับ
เครื่องเล่นในรถที่บังคับใส่ `0000` + ลดเวลาเข้าโหมดตั้งค่าจาก 180 s เหลือ **60 s** —
flash ผ่านด้วย `--before default_reset` แล้ว และตรวจ boot log บนบอร์ดจริงเรียบร้อย
เหลือทดสอบจับคู่กับเครื่องเล่นในรถจริง)

อ่าน `README.md` สำหรับผังสาย/ตัวเลือก/menuconfig ทั้งหมด — ไฟล์นี้สรุปแค่ว่า
**ทำอะไรเสร็จแล้ว, อะไรยังไม่ได้ทำ, และเริ่มต่อยังไง**

## รอบล่าสุด (30 ต.ค. 2026) — รหัส PIN จับคู่ + portal 60 s

**ทำเสร็จ (build ผ่าน, flash ลงบอร์ดแล้ว, ยืนยันจาก boot log)**

- **รหัส PIN จับคู่แบบ Legacy** เป็นค่าตั้งค่าแล้ว (`app_settings.h`: `char pin_code[17]`
  = 1–16 หลัก): `validate()` เปลี่ยนค่าที่ไม่ใช่ตัวเลขหรือยาวเกินเป็นค่า Kconfig
  (`CONFIG_APP_BT_PIN_CODE`, ค่าเริ่มต้น `1234`) ⇒ ตั้ง `0000` สำหรับเครื่องเล่นในรถ
  ได้จากหน้าเว็บ ไม่ต้อง build ใหม่
- ส่งให้ Bluetooth stack ก่อน `start()` ด้วย `set_pin_code(pin, ESP_BT_PIN_TYPE_FIXED)`
  (`main/a2dp_source.cpp`) ⇒ controller ตอบรหัสเองทันทีเมื่อ sink ขอ PIN แบบ legacy
  (ไม่ต้องรอ event ของ app) — log: `a2dp_src: pairing: legacy PIN '1234'`
- หน้าเว็บมีช่อง “รหัส PIN สำหรับจับคู่แบบ Legacy (1–16 หลัก)” (ตรวจรูปแบบก่อน POST),
  `/api/state` ส่ง `settings.pin`, `POST /api/settings` รับ `pin`
- **portal เร็วขึ้น**: `CONFIG_APP_PORTAL_DELAY_S` 180 → **60** — ต้องแก้ทั้ง default ใน
  `Kconfig.projbuild` และค่าใน `sdkconfig` ไม่งั้น build ยังใช้ 180
- `SETTINGS_VERSION` 1 → 2 ⇒ ค่าที่เคยเก็บใน NVS (ชื่อลำโพง ฯลฯ) รีเซ็ตเป็น Kconfig
  หนึ่งครั้งหลังแฟลช

**ยืนยันบนบอร์ดจริง (boot log จาก `monitor2.log`)**

| สิ่งที่ทดสอบ | ผล |
|---|---|
| โหลด PIN จาก NVS | ✅ `settings: loaded: ..., PIN '1234', SSP off, ...` |
| ส่ง PIN เข้า stack ก่อน start | ✅ `a2dp_src: pairing: SSP disabled (legacy PIN only)` + `pairing: legacy PIN '1234'` |
| portal หลัง 60 s | ✅ `waiting for a speaker (config portal opens after 60 s)` → `no speaker after 60 s: switching to the config portal` (~61.8 s) |
| ปลด BT แล้ว AP ขึ้น | ✅ `Bluetooth released (126580 bytes free)` → `access point 'ESP32-A2DP-Setup' up` / `config portal ready (146844 bytes free)` |
| จับคู่กับเครื่องเล่นในรถจริง | ❌ ยังไม่ได้ (ยังไม่มีรถ/ฮาร์ดแวร์ในรอบนี้) |

## รอบก่อน — Wi-Fi config portal (AP mode)

**ทำเสร็จ (build ผ่าน, image 1.4 MB, `firmware.bin` รวมทุกอย่างแล้ว)**

- ค่าตั้งค่าย้ายจาก compile-time → NVS (`app_settings.c`, namespace `appcfg`):
  target name / local name / **pairing PIN** / SSP / I2S input rate / buffer ms —
  Kconfig เป็นแค่ค่าโรงงาน (NVS version 2)
- AP `ESP32-A2DP-Setup` (`esp32cam`) + HTTP server (`config_portal.c`) + หน้าเว็บ
  ฝังในเฟิร์มแวร์ (`main/web/index.html`, `EMBED_FILES`)
  - `GET /` หน้าเว็บ, `GET /api/state`, `POST /api/settings`, `POST /api/rescan`,
    `POST /api/reboot`
  - รายชื่อลำโพงมาจาก discovery ของ A2DP เอง (`bt_scan.c` เก็บชื่อ/addr/RSSI 8 ตัว)
  - หน้าตั้งค่า: เลือกลำโพง, ชื่อบอร์ด, รหัส PIN จับคู่, SSP, input rate, buffer ms
    (A2DP output ตรึง 44100 — SBC ของ Bluedroid ฮาร์ดโค้ด `btc_a2dp_source.c:446`)
- ลำดับการทำงาน (แก้รอบล่าสุด): **บูตเข้า Bluetooth อย่างเดียว ไม่เปิด Wi-Fi ตั้งแต่แรก**
  → ถ้าไม่ต่อลำโพงได้ภายใน `CONFIG_APP_PORTAL_DELAY_S` (ค่าเริ่มต้น **60 s**) 
  → `a2dp_source_stop()` = `end(true)` ปิด Bluedroid/controller + คืนหน่วยความจำ
  Classic BT → คืน ring buffer 64 KB → เปิด AP + HTTP server (**ไม่มีวันกลับไป BT
  ในรอบบูตนั้น**, reboot เท่านั้น — ซึ่งคือสิ่งที่ POST /api/settings ทำ)
  (บิลด์ก่อนหน้าเปิด AP 3 s หลังบูตทับ Bluetooth แล้วปิดเมื่อเริ่มเชื่อมต่อ — เอาออกแล้ว)
- boot-time pin check (`pin_activity.c`): sample BCLK/WS/DATA 60 ms แล้วบอกว่า
  toggling / static HIGH / static LOW

**ยืนยันบนบอร์ดจริงแล้ว (ตอนนั้น portal ยังตั้งไว้ 180 s)**

| สิ่งที่ทดสอบ | ผล |
|---|---|
| บูตแล้วไม่เปิด Wi-Fi | ✅ `app: waiting for a speaker (config portal opens after 180 s)`, ไม่มี `access point ... up` |
| ครบ 180 s แล้วสลับโหมด | ✅ `no speaker after 180 s: switching to the config portal` (ต่อมาลดเหลือ 60 s แล้ว) |
| ปล่อย Bluetooth คืน heap | ✅ `a2dp_src: Bluetooth released (126584 bytes free)` → `audio buffer released (192124 bytes free)` |
| AP ขึ้นหลัง BT ดับ | ✅ `access point 'ESP32-A2DP-Setup' up` / `config portal ready (146824 bytes free)` |
| ต่อ AP จาก Windows + GET/POST จริง | ✅ ต่อ `ESP32-A2DP-Setup` ได้ และใช้งานหน้าเว็บ/API ได้ (รอบก่อนก็ผ่าน) |
| buffer ใหญ่เกินจะจอง contiguous | ✅ เจอบั๊ก: `buffer_ms 400` → ขอ 131072 B แล้ว `app: audio buffer init failed - stopping` ⇒ แก้เป็นลดขนาดลงครึ่งหนึ่งจนจองได้ (`only ... reduced to 371 ms`) |

**ยืนยันในรอบก่อน (ตอนที่ portal เปิดทับ Bluetooth)**

| สิ่งที่ทดสอบ | ผล |
|---|---|
| settings เขียน/อ่าน NVS ข้ามบูต | ✅ `settings: loaded: sink 'BT Speaker', ... buffer 300 ms` |
| AP + HTTP server ขึ้น | ✅ `access point 'ESP32-A2DP-Setup' up` / `config portal ready (75780 bytes free)` |
| คืน ring buffer ตอนเข้าโหมดตั้งค่า | ✅ `audio buffer released for the config portal (122036 bytes free)` |
| ~~ปิด portal เมื่อเริ่มต่อลำโพง~~ | ❌ ไม่มีแล้ว (path นี้ถูกแทนที่ด้วย "ปิด Bluetooth ก่อนเปิด portal") |
| discovery → รายชื่อลำโพง | ✅ `bt_scan: sink BT Speaker (00:58:56:74:31:f3, -35 dBm)` |
| pin check ตอนบูต | ✅ `BCLK: toggling, 44456 edges` / `WS: toggling, 5297` |
| หน้าเว็บ (Chrome headless + mock API) | ✅ เรนเดอร์สถานะ/รายชื่อ/RSSI ถูก, escape ชื่อที่มีอัญประกาศได้ |
| ต่อ AP จาก Windows + GET/POST จริง | ❌ ยังไม่ได้ (ติดข้อล่าง) |

**บั๊กที่เจอจากการรันและแก้แล้ว**

1. `httpd_create: Failed to allocate memory for HTTP session data` — default 7 sockets
   (~9 KB contiguous) ไม่พอ ⇒ `max_open_sockets=2`, `max_resp_headers=4`, stack 4096
2. มี client ต่อแล้ว heap เหลือ **2 248 ไบต์** ⇒ `httpd_sock_err: error in send` (ได้
   200 แต่ body 0 ไบต์) ⇒ คืน ring buffer 64 KB ตอนเข้าโหมดตั้งค่า, ลด TCP window
   (lwIP 2880), `LWIP_MAX_SOCKETS=8`
3. `max_open_sockets=2` ถูกปฏิเสธเมื่อ `LWIP_MAX_SOCKETS=4` (server กันไว้ 3)
4. ลด `MGMT_SBUF_NUM` 32→8 + dynamic buffer 8→4 ทำให้ **AP ไม่ส่ง beacon** (มือถือ
   สแกนไม่เห็น SSID) ⇒ คืนค่าเดิม อย่าลดต่ำกว่านี้
5. Wi-Fi IRAM opt เปิดอยู่ทำให้ลิงก์ไม่ผ่าน (`iram0_0_seg overflowed by 720 bytes`)
   ⇒ ปิด `ESP_WIFI_IRAM_OPT` / `ESP_WIFI_RX_IRAM_OPT`

**แฟลชแล้ว — ไม่มีอะไรค้าง (30 ต.ค. 2026)**

- `firmware.bin` ใหม่: **1,503,616 bytes (0x16f180)**, MD5 `f6eec369aaaa6ed1a48d8031c34b2412`
  (ตรวจแล้วว่า bootloader `0x1000` / partition table `0x8000` / app `0x10000`
  ตรงกับไฟล์ใน `build/` ทุกช่วง)
- แฟลชผ่านด้วย **`--before default_reset`** (รอบนี้ DTR/RTS ทำงาน — ไม่ต้องจั๊ม IO0 แล้ว):
  `Wrote 1503616 bytes (904257 compressed) at 0x00000000 ... Hash of data verified.`
  ส่วน `--before no_reset` ยังล้ม (`No serial data received`) เพราะชิปรันแอปอยู่
- boot log ตรงตามที่ต้องการ: `settings: loaded ... PIN '1234'` → `pairing: legacy PIN '1234'`
  → `config portal opens after 60 s` → `no speaker after 60 s` → AP + HTTP ขึ้น
- **ข้อควรระวัง**: `firmware.bin` เติมช่วง NVS ด้วย 0xFF ⇒ ค่าตั้งค่า/คู่ BT เดิมหาย
  บอร์ดกลับไปใช้ Kconfig (`BT Speaker`, PIN `1234`) — ต้องเข้า portal ไปตั้ง PIN `0000`
  ก่อนใช้งานกับรถ
- **หมายเหตุเครื่องมือ**: `idf.py monitor` บนเครื่องนี้ crash ด้วย
  `UnicodeEncodeError: 'charmap' codec ... cp874` เมื่อ redirect ลงไฟล์ ⇒ ต้องตั้ง
  `PYTHONUTF8=1 PYTHONIOENCODING=utf-8` (log ที่ถูกต้องอยู่ที่ `monitor2.log`)

## ภาพรวมสถานะ

| ส่วน | สถานะ |
|---|---|
| โค้ดบริดจ์ I2S → A2DP (`main/`) | ✅ เขียนครบ, build ผ่าน |
| Flash ลงบอร์ด COM6 | ✅ ผ่าน (image 1,503,616 B, `default_reset`) |
| บูต + Bluetooth Classic + A2DP/AVRCP | ✅ ผ่าน |
| Self-test เส้นทางรับข้อมูล (loopback, 44.1k) | ✅ PASS (128 frames, L/R ถูก) |
| Self-test เส้นทาง resample (loopback, 48k→44.1k) | ✅ PASS (117 frame ramp, ~3.27/frame) |
| Self-test resampler 48k→44.1k (ระดับฟังก์ชัน) | ✅ PASS (1880/1881.6 frames) |
| จับคู่ + stream ไปลำโพง "BT Speaker" จริง | ⚠️ จับคู่ได้, `BT=Streaming`, tone ป้อนข้อมูลแล้ว — แต่เสียงยังไม่ได้ยืนยันด้วยหู และ link หลุดหลัง ~15 s (heap หมด) |
| รับ PCM จาก ESP32-S3 จริง | ❌ ยังไม่ได้ทดสอบ (ยังไม่ต่อสาย) |
| Wi-Fi config portal (AP + หน้าเว็บ) | ✅ แฟลชลงบอร์ดแล้ว, portal ขึ้นที่ 60 s ตาม log (HTTP GET/POST จาก client จริงยังไม่ได้ทดสอบรอบนี้) |
| รหัส PIN จับคู่แบบ Legacy | ⚠️ ตั้ง/บันทึกผ่านหน้าเว็บ–API ได้, log ยืนยันตอนบูต — ยังไม่ได้จับคู่กับเครื่องเล่นในรถจริง |
| Soak test 30 นาที | ❌ ยังไม่ได้ทำ |
| แก้ฝั่งโปรเจกต์ S3 (`setOutputSampleRate`) | ❌ ยังไม่ได้แก้ (อยู่นอก repo นี้) |

## สิ่งที่ต้องทำต่อ (เรียงตามลำดับ)

1. ~~ใส่ชื่อลำโพงจริง~~ — ตั้งเป็น `CONFIG_APP_BT_TARGET_NAME="BT Speaker"` แล้ว
   (จับกับ `00:58:56:74:31:f3`) และเปิด *Use a generated 1 kHz test tone* ไว้
2. **ยืนยันเสียงด้วยหู** — เปิดลำโพงไว้ตอน flash/boot ควรได้ยินโทน 1 kHz ต่อเนื่อง;
   รอบที่ทดสอบล่าสุด stream ได้ ~10 s แล้ว audio state กลายเป็น suspend/disconnect
   (heap เหลือ 4 KB เพราะ stack จอง TX buffer 4112 B × 15 ไม่ drain — ดูหัวข้อ
   "ปัญหาที่ยังเปิดอยู่") ลองเปิดลำโพงใหม่/ขยับให้ใกล้ขึ้นแล้ว capture log ดูว่า
   `BT=Streaming` ต่อเนื่องหรือไม่
   (ถ้าจับคู่ไม่ติดให้เปิด *Use Secure Simple Pairing (SSP)* ดูหัวข้อ 2.1 ใน README)
3. **ต่อสาย S3 → CAM** ตามผังใน README (GPIO1/2/40 → CAM 14/13/4 + GND ร่วม)
   แล้วค่อยปิด test tone กลับมาใช้ I2S
4. **แก้ฝั่ง S3** เพิ่ม `audio.setOutputSampleRate(Audio::SR_44100);` ใน `setup()`
   (ดูหัวข้อ "สิ่งที่ต้องแก้ฝั่ง ESP32-S3" ใน README)
5. **ทดสอบระบบจริง** — ตรวจ `BT=Streaming` และ `I2S ~176400 B/s`, ฟังเสียง,
   ทดสอบถอด/ต่อลำโพง (auto-reconnect), แล้วทดสอบต่อเนื่อง 30 นาที
   (ดู `drop`/`underflow` ใน log)

## สภาพแวดล้อม/build บนเครื่องนี้

ESP-IDF v5.5.6 ติดตั้งที่ `C:\esp\esp-idf`, tools ที่ `C:\esp\espressif`,
Python 3.12 (uv) — `idf.py` ตรง ๆ จะพังถ้าเรียกจาก Git Bash (มี `MSYSTEM`)
จึงมี wrapper เตรียมไว้:

```bash
cd esp32cam_i2s_a2dp
source /c/esp/idf-env.sh                          # ตั้ง PATH/IDF_PATH
/c/esp/espressif/python_env/idf5.5_py3.12_env/Scripts/python.exe \
    /c/esp/run_idf.py -p COM6 flash monitor        # build+flash+monitor
```

`components/` (ESP32-A2DP + arduino-audio-tools) ถูก `.gitignore` ไว้ — ถ้า clone
ใหม่ต้องคัดลอก/clone ตามหัวข้อ "ติดตั้ง dependency" ใน README ก่อน build

## ค่าที่ตั้งไว้ใน `sdkconfig` ปัจจุบัน

```
CONFIG_APP_BT_TARGET_NAME="BT Speaker"   # ตั้งตามลำโพงที่ทดสอบ
CONFIG_APP_BT_PIN_CODE="1234"            # ตั้ง 0000 จากหน้าเว็บเมื่อใช้กับรถ
# CONFIG_APP_BT_SSP is not set             # เปิดถ้าลำโพงรุ่นใหม่ต้องการ SSP
CONFIG_APP_PORTAL_DELAY_S=60             # 1 นาที (เดิม 180)
CONFIG_APP_OUTPUT_RATE_44100=y
CONFIG_APP_INPUT_RATE_44100=y
CONFIG_APP_I2S_BCLK_GPIO=14  WS=13  DATA=4
CONFIG_APP_I2S_PCM_SHIFT_BITS=16
CONFIG_APP_AUDIO_BUFFER_MS=300           # ได้จริง 16384 frames = 371 ms
# CONFIG_APP_LOOPBACK_TEST is not set     # เปิดชั่วคราวเพื่อ self-test
CONFIG_APP_TEST_TONE=y                   # เปิดอยู่ระหว่างทดสอบลำโพง
# CONFIG_SPIRAM is not set                # บอร์ดมี PSRAM แต่ยังไม่เปิด (ไม่จำเป็น)
```

## บั๊กที่แก้ไปแล้วในรอบนี้

**(รอบทดสอบกับลำโพงจริง)**

1. **ไม่เคยมีเสียงออกเลย** — `BluetoothA2DPSource::get_audio_state()` คืนค่า cache ที่
   ไลบรารีอัปเดต *เฉพาะเมื่อมีคนลงทะเบียน `set_on_audio_state_changed()`* เท่านั้น
   เราไม่ได้ลงทะเบียน ⇒ ค่าค้างที่ `ESP_A2D_AUDIO_STATE_SUSPEND` ตลอด
   ⇒ `a2dp_source_is_streaming()` = false ตลอด ⇒ ทั้ง tone source และ I2S receiver
   ไม่เคยป้อน PCM (log: `BT=Connected (suspended)` + `underflow` เพิ่มไม่หยุด)
   แก้: ลงทะเบียน callback (no-op ที่แค่ log) ใน `main/a2dp_source.cpp`
2. **panic ตอน tone เริ่มทำงาน** — `heap_caps_malloc(size, MALLOC_CAP_INTERNAL)`
   คืนที่อยู่ใน IRAM ได้ ซึ่งรับได้เฉพาะ access 32-bit ⇒ `s16i` (store 16-bit) ทำให้
   `LoadStoreError` (PC ที่ `tone_source.c:53`, EXCVADDR 0x40099054)
   แก้: ใช้ `MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT` เหมือน `audio_buffer.c`

**(รอบก่อน)**

บอร์ดจำ peer เก่าใน NVS (`a7:d3:a7:f2:59:97`) แล้ววน reconnect หา peer นั้น
ไม่ยอมค้นหาลำโพงที่ตั้งไว้ ⇒ `main/main.cpp` ล้าง peer ที่เก็บไว้ตอนบูตเมื่อมี
การระบุชื่อลำโพง (log: `a2dp_src: cleared the stored peer address`)

## สิ่งที่เพิ่งทำในรอบนี้

- เพิ่มตัวเลือก **Use Secure Simple Pairing (SSP)** (`CONFIG_APP_BT_SSP`, ค่าเริ่มต้น
  ปิด) ใน `main/Kconfig.projbuild` → `APP_BT_SSP_ENABLED` → เรียก
  `set_ssp_enabled()` **ก่อน** `start()` ใน `main/a2dp_source.cpp` เพื่อให้ลำโพงรุ่น
  ใหม่ที่เลิกใช้ Legacy PIN จับคู่ได้; ตอนบูต log บอกสถานะ
  (`a2dp_src: pairing: SSP enabled (numeric value auto-confirmed)`)
- บอร์ดไม่มีจอ/คีย์แพด ⇒ numeric comparison ถูก auto-confirm, ส่วน passkey ที่ต้อง
  กรอกจะถูก log เฉย ๆ (ข้อจำกัดของไลบรารี)
- ทดสอบบนบอร์ดจริง: build/flash ผ่านทั้ง `CONFIG_APP_BT_SSP=y` และค่าเริ่มต้น (ปิด),
  boot ขึ้น `A2DP Enable with AVRC` ปกติ heap ~71 KB เท่าเดิม แล้ว flash กลับเป็น
  ค่า default (SSP ปิด)
- ยัง**ไม่ได้**ทดสอบจับคู่กับลำโพงจริง (ไม่มีฮาร์ดแวร์)

- เพิ่มการตรวจเส้นทาง resample ใน `main/loopback_test.c`: เมื่อ *I2S input sample
  rate* ≠ *A2DP sample rate* ตัวรับจะ resample จริง self-test จึงเปลี่ยนไปตรวจ
  "ความหนาแน่น" ของ ramp (จำนวน frame ≈ 128 × out/in, step 3..4 ต่อ frame)
  พร้อมเช็ค R = -L ทุก frame แทนการเทียบค่าตรง ซึ่งใช้ไม่ได้เพราะ interpolation
  เริ่มที่ตำแหน่งเศษส่วนได้ (`LOOPBACK PASS (resampled 48000 -> 44100 Hz): ...`)
- ทดสอบบนบอร์ดจริงทั้งสองแบบ: 44100→44100 PASS (128 frames) และ 48000→44100
  PASS (117 frame ramp) แล้ว flash กลับเป็นค่า default (44100, loopback ปิด)
- ยัง**ไม่ได้**ทดสอบเสียงจริงกับลำโพง Bluetooth หรือ S3 (ไม่มีฮาร์ดแวร์)

## ปัญหาที่ยังเปิดอยู่ (รอบทดสอบกับลำโพงจริง)

หลังเริ่ม stream ได้ ~10–15 s: heap ลดจาก ~87 KB เหลือ 4 KB แล้ว link หลุด

```
E (36283) BT_OSI: malloc failed (caller=0x40101487 size=4112)
E (36284) BT_OSI: heap info: free=4132, largest_block=3328
E (36284) BT_APPL: ERROR btc_media_aa_prep_sbc_2_send no buffer TxCnt 15
I (40966) a2dp_src: audio state: suspended/stopped
I (41332) app: BT=Disconnected ... heap 92952
```

- `btc_media_aa_prep_sbc_2_send()` จอง `BT_HDR` 4112 B ต่อ media packet; ถ้า peer ไม่
  ACK/ไม่ drain คิว `TxAaQ` จะเต็ม 15 ใบ = ~62 KB ⇒ heap หมด ⇒ ส่งเสียงไม่ได้
- ช่วงที่ยังดี: `BT=Streaming`, `underflow` คงที่ 1317 frames, buffer 2–16%
  ⇒ tone ป้อน PCM ทันและ BT บริโภคทันที (พิสูจน์ว่ามีข้อมูลจริงไหลไปหาลำโพง)
- ต้องแยกให้ออกว่าเป็น (ก) ลำโพงเองหยุดรับ/ปิดตัว (ข) link อ่อน (RSSI -60)
  หรือ (ค) heap เหลือน้อยเกินไปสำหรับ streaming (หลังเปิด tone heap เหลือ ~87 KB)
- แนวทางที่ลองต่อได้: ลด `Audio buffer` จาก 300 ms → 150 ms (คืน ~32 KB),
  เปิด `CONFIG_APP_BT_SSP` เมื่อลำโพงใหม่ไม่รับ legacy, ลด log level ตอนทดสอบ

## ข้อสังเกตที่เจอตอนทดสอบ (ยังไม่ต้องแก้)

- `timeouts` ใน log เพิ่มตลอดเมื่อไม่มี S3 ต่ออยู่ — **ปกติ** เพราะไม่มี BCLK
- `app: BT=Disconnected | I2S 0 B/s | buf 0% | heap ~71 KB` คือสภาพ idle ปกติ
- Discovery วนซ้ำทุก ~12.8 วินาทีเมื่อหาลำโพงไม่เจอ — พฤติกรรมของไลบรารี
