# ESP32 Internet Radio → Bluetooth A2DP

ฟังสถานีวิทยุอินเทอร์เน็ตผ่านลำโพง Bluetooth ด้วยบอร์ด **ESP32-WROVER** ตัวเดียว
(ไม่ต้องมี ESP32-S3 / สาย I2S / PCM5102A แล้ว)

```
สถานีวิทยุ (HTTP/HTTPS MP3)
      │  esp_http_client
      ▼
 MP3 decoder (Helix, fixed point)
      │  PCM16 stereo
      ▼
 แปลงอัตราสุ่มเสียง → 44100 Hz (ถ้าสถานีส่งอัตราอื่น)
      │
      ▼
 jitter buffer (PSRAM, 200–3000 ms)
      │  data callback ของ Bluetooth
      ▼
 Bluetooth A2DP Source ──► ลำโพง Bluetooth
```

โปรเจกต์นี้สร้างจาก `../esp32cam_i2s_a2dp` โดย**ลบส่วนรับสัญญาณ I2S ออกทั้งหมด**
(`i2s_receiver`, `resampler` เดิมถูกใช้ต่อเฉพาะส่วนแปลงอัตรา, `loopback_test`,
`pin_activity`, `tone_source` ถูกลบ) และเปลี่ยนต้นทางเสียงจาก I2S มาเป็นสตรีม MP3
ผ่าน Wi-Fi แทน

## โครงสร้างไฟล์

| ไฟล์ | หน้าที่ |
|---|---|
| `main/main.cpp` | จุดเริ่มระบบ: โหลดค่าตั้ง, เปิด Wi-Fi, config page, Bluetooth, วิทยุ, ล็อกสถานะ |
| `main/radio_stream.c` | Task เดียว: เปิดสตรีม HTTP(S), ถอดรหัส MP3, แปลงอัตรา, เขียนลง jitter buffer, reconnect อัตโนมัติ |
| `main/wifi_link.c` | ต่อ Wi-Fi (STA) + เปิด AP สำรอง `ESP32-Radio-Setup` เมื่อต่อไม่ได้ |
| `main/config_portal.c` | HTTP server หน้าเว็บตั้งค่า (`/`, `/api/state`, `/api/settings`, `/api/rescan`, `/api/reboot`) |
| `main/web/index.html` | หน้าเว็บ (ฝังในเฟิร์มแวร์ด้วย `EMBED_FILES`) |
| `main/a2dp_source.cpp` | ครอบ `BluetoothA2DPSource` (ค้นหา/จับคู่/เชื่อมต่อ/รับ PCM จาก buffer) |
| `main/audio_buffer.c` | Ring buffer แบบ SPSC (อยู่ใน PSRAM) |
| `main/resampler.c` | แปลงอัตรา 48k/32k/22.05k → 44.1k (linear interpolation) |
| `main/app_settings.c` | เก็บค่าตั้งใน NVS (URL, Wi-Fi, ชื่อ/ PIN Bluetooth, buffer) |
| `main/bt_scan.c` | แคชรายชื่อลำโพงที่ค้นพบ ให้หน้าเว็บแสดง |
| `main/Kconfig.projbuild` | ค่าโรงงาน (menuconfig) |

`components/` ไม่ได้ copy มา: `CMakeLists.txt` ของโปรเจกต์ชี้ไปที่
`../esp32cam_i2s_a2dp/components` (ESP32-A2DP + arduino-audio-tools) เพื่อไม่ให้เก็บ
ไลบรารีชุดเดียวกันซ้ำอีกราว 120 MB — ถ้าจะย้ายโปรเจกต์ไปที่อื่นให้ copy โฟลเดอร์
`esp32cam_i2s_a2dp/components` ตามไปด้วย (หรือแก้ `EXTRA_COMPONENT_DIRS`)

MP3 decoder (`chmorgan/esp-libhelix-mp3`) เป็น managed component:
build ครั้งแรกต้องต่ออินเทอร์เน็ตเพื่อดาวน์โหลดลง `managed_components/`
หลังจากนั้น build แบบ offline ได้

## ความต้องการ

- บอร์ด ESP32-WROVER (ESP32 รุ่นแรกที่มี Bluetooth Classic), PSRAM 8 MB, flash 4 MB
  (ถ้า flash 8 MB ให้แก้ `CONFIG_ESPTOOLPY_FLASHSIZE_8MB` ได้)
- ESP-IDF **5.5.6** (บนเครื่องนี้อยู่ที่ `C:\esp\esp-idf`, tools ที่ `C:\esp\espressif`)
- ลำโพง/หูฟัง Bluetooth ที่รองรับ A2DP

## Build / Flash

จาก **cmd.exe** (วิธีปกติของ ESP-IDF):

```bat
C:\esp\esp-idf\export.bat
cd /d %USERPROFILE%\InternetRadioToBluetooth\esp32radio_a2dp
idf.py set-target esp32
idf.py build
idf.py -p COM5 flash monitor     :: เปลี่ยน COM5 ตามบอร์ดจริง
```

`export.bat`/`export.sh` ของ IDF 5.5 จะปฏิเสธ Git Bash (MSys) จึงต้องใช้ cmd.exe
ถ้าจำเป็นต้อง build จาก bash จริง ๆ ให้เรียก CMake ตรง ๆ (ข้ามการเช็ก Python
dependencies ที่ไม่รองรับ MSys):

```bash
export IDF_PATH=/c/esp/esp-idf
export IDF_TOOLS_PATH=/c/esp/espressif
export PATH="/c/esp/espressif/tools/xtensa-esp-elf/esp-14.2.0_20260121/xtensa-esp-elf/bin:\
/c/esp/espressif/tools/cmake/3.30.2/bin:/c/esp/espressif/tools/ninja/1.12.1:\
/c/esp/espressif/python_env/idf5.5_py3.12_env/Scripts:$PATH"
cmake -G Ninja -S . -B build \
  -DCMAKE_TOOLCHAIN_FILE=$IDF_PATH/tools/cmake/toolchain-esp32.cmake \
  -DIDF_TARGET=esp32 -DPYTHON_DEPS_CHECKED=1
ninja -C build
```

## ตั้งค่าผ่านหน้าเว็บ

หน้าเว็บทำงานตลอดเวลา (ไม่ใช่โหมดแยก) จึงแก้ URL สถานีได้แม้กำลังเล่นเพลงอยู่

1. บอร์ดต่อ Wi-Fi ที่ตั้งไว้ → เข้า `http://<IP ของบอร์ด>/` จากมือถือ/คอมในวงเดียวกัน
   (IP จะขึ้นใน Serial monitor ตอนบูต)
2. ถ้ายังไม่ได้ตั้ง Wi-Fi หรือต่อไม่ได้เกิน 25 วินาที บอร์ดจะเปิด AP สำรอง
   **ESP32-Radio-Setup** รหัส **esp32radio** → เข้า http://192.168.4.1/
   แล้วกรอก SSID/รหัส Wi-Fi ของบ้าน
3. ฟิลด์ที่มีให้ตั้ง:
   - **รายการสถานี** – เพิ่มได้สูงสุด 20 สถานี (ชื่อ + ลิงก์ MP3) มีปุ่ม “ลบ” ต่อรายการ
     และช่อง “＋ เพิ่มสถานี” ด้านล่าง · ปุ่ม Next/Previous บนลำโพงจะวนตามลำดับนี้
     (สถานีที่กำลังเล่นมีป้าย “กำลังเล่น”)
   - **Wi-Fi SSID / รหัสผ่าน**
   - **ชื่อลำโพง Bluetooth** (เว้นว่าง = ต่อตัวแรกที่เจอ), ชื่อบอร์ด, PIN, SSP
   - **ขนาด jitter buffer** 200–3000 ms
   - **ระดับเสียงตอนเริ่มเล่น** 0–127 (ส่งให้ลำโพงทุกครั้งที่เริ่มสตรีม)
4. กด **บันทึกและรีสตาร์ท** — ค่าถูกเก็บใน NVS แล้วบอร์ดรีสตาร์ท 1 รอบ

ค่าโรงงาน (แก้ได้ใน `menuconfig` → *ESP32 Internet radio to A2DP source*):

| ค่า | ค่าเริ่มต้น |
|---|---|
| `APP_STATION_URL` | `https://coolism-web.cdn.byteark.com/;stream/1` |
| `APP_STATION_NAME` | `COOLfahrenheit` |
| `APP_WIFI_SSID` / `APP_WIFI_PASSWORD` | ว่าง (บูตเข้าโหมด AP) |
| `APP_BT_DEVICE_NAME` | `ESP32-Radio-A2DP` |
| `APP_BT_TARGET_NAME` | `BT-Speaker` |
| `APP_BT_PIN_CODE` | `1234` |
| `APP_AUDIO_BUFFER_MS` | `1000` |
| `APP_INITIAL_VOLUME` | `60` (0–127, ระดับเสียงที่ส่งให้ลำโพงเมื่อเริ่มเล่น) |

## ลำดับการทำงาน

1. โหลดค่าจาก NVS → สร้าง jitter buffer (PSRAM) → เริ่ม Wi-Fi
   (STA ถ้ามี SSID, ไม่งั้น AP), เริ่ม HTTP server
2. เริ่ม Bluetooth A2DP Source → ค้นหา/เชื่อมต่อลำโพง (SSP หรือ PIN ตามค่าตั้ง)
3. เมื่อลำโพง**เชื่อมต่อแล้ว** main loop สั่งให้ task วิทยุเปิดสตรีม
4. Task วิทยุอ่านข้อมูลจาก socket → หา sync word → ถอดรหัส MP3 ทีละเฟรม →
   แปลงเป็น stereo 44100 Hz → เขียนลง buffer (ถ้า buffer เต็มจะรอสูงสุด 500 ms
   แล้วทิ้งเฟรมนั้น)
5. A2DP data callback ดึง PCM จาก buffer; ถ้าไม่พอจะเติมความเงียบ (นับเป็น underflow)
6. ลำโพงหลุด → ปิดสตรีมและล้าง buffer (ฟังวิทยุสด ไม่เล่นย้อนหลัง), ต่อใหม่แล้วเปิดสตรีมใหม่
7. สตรีมหลุด/สถานีล่ม → reconnect อัตโนมัติ (หน่วง 2 s เพิ่มขึ้นถึง 15 s)

## หน่วยความจำ

- jitter buffer, บัฟเฟอร์ถอดรหัส และบัฟเฟอร์ TLS อยู่ใน **PSRAM**
  (internal RAM เหลือให้ Bluetooth/Wi-Fi)
- `sdkconfig.defaults` ปิด Wi-Fi IRAM optimization และ BLE เพื่อให้ Bluetooth
  อยู่ใน IRAM ได้ (ผล build: IRAM 93%, DRAM 73% — เหลือ IRAM ~9 KB อย่าเปิด
  ฟีเจอร์ที่กิน IRAM เพิ่ม)
- พาร์ทิชัน app 3 MB, ไบนารีราว 1.7 MB

## การควบคุมจากปุ่มบนลำโพง (AVRCP)

บอร์ดลงทะเบียนเป็น **AVRCP Target** ให้ลำโพงส่งคำสั่งผ่าน Bluetooth ได้
(ลำโพงต้องรองรับ AVRCP ซึ่งลำโพง Bluetooth ส่วนใหญ่มี)

| ปุ่มบนลำโพง | ผลกับวิทยุ |
|---|---|
| Play / ▶ | เล่นต่อ (ต่อสตรีมใหม่ที่จุดสด) |
| Pause / ❚❚ | หยุดชั่วคราว — ปิด socket เพื่อไม่ให้มีเสียงค้างย้อนหลัง |
| Stop / ■ | เหมือน Pause |
| Next / ⏭ | สถานีถัดไปในรายการ (วนกลับไปสถานีแรกเมื่อสุดรายการ) |
| Previous / ⏮ | สถานีก่อนหน้า (วนไปสถานีสุดท้ายเมื่ออยู่ที่แรก) |

ปุ่มอื่น (เช่น Volume, FF/REW) จะถูกบันทึกใน log และไม่ทำอะไร
สถานะจะขึ้นใน Serial ว่า `paused (remote)` และหน้าเว็บจะแสดงตามนั้น

## ผลการตรวจสอบ (ตามจริง)

ตรวจแล้ว:

- `cmake` configure + `ninja` build ผ่านด้วย ESP-IDF 5.5.6, target esp32
  ไม่มี error/warning จากไฟล์ที่เขียนใหม่
- ตรวจ header ของสถานีตัวอย่างด้วย `curl`: `HTTP 200`, `Content-Type: audio/mpeg`,
  `icy-br: 128`, `icy-sr: 44100`, เฟรม MP3 เริ่มด้วย `FF FB` (MPEG1 Layer III,
  128 kbps, 44.1 kHz, joint stereo) → ตรงกับสมมติฐานของไปป์ไลน์ (ไม่ต้องรีแซมเปิล)
- **ทดสอบบนบอร์ดจริง (COM6, ESP32-WROVER 8 MB PSRAM):** บูตผ่าน, ตรวจ PSRAM
  8 MB, config page ขึ้นที่พอร์ต 80, Wi-Fi ต่อ `spd_wifi` ได้ (DHCP 192.168.8.67),
  Bluetooth ต่อลำโพง `BT Speaker` ได้, สตรีมเปิดผ่าน HTTPS (TLS + cert bundle),
  ถอดรหัสได้ `128 kbps 44100 Hz 2ch` และเล่นออกลำโพงได้จริง
- จากผลทดสอบบนบอร์ดพบและแก้ 3 จุด:
  1. `esp_wifi_init()` ปล่อยโหมดเป็น `WIFI_MODE_NULL` → ต้องตั้ง `WIFI_MODE_STA`
     ก่อน `esp_wifi_set_config()` (เดิม Wi-Fi ไม่เริ่มหลังรีบูต)
  2. `wifi_link_poll()` เรียก `esp_wifi_connect()` ซ้ำขณะรอ DHCP ทำให้ไม่ได้ IP
     → retry เฉพาะเมื่อ station หลุดจริง
  3. ค่า timeout ของ AP สำรองคำนวณหน่วยผิด (25 s กลายเป็น 25 ms) → AP เปิดทุกครั้ง
     และแย่งเวลา radio กับ Bluetooth; แก้เป็นไมโครวินาที + ปิด AP อัตโนมัติเมื่อได้ IP

ยังไม่ได้ทดสอบ: การเล่นต่อเนื่องหลายชั่วโมง, คุณภาพเสียงเชิงลึก (THD/glitch meter),
และการกดปุ่มจริงบนลำโพง (โค้ดลงทะเบียน AVRCP Target แล้ว แต่ยังไม่ได้กดทดสอบ)

## แก้ปัญหาที่พบระหว่างทดสอบ

- **ลำโพงไม่ถูกเลือกเพราะชื่อไม่ตรง:** ค่าเริ่มต้นเดิม `BT-Speaker` ไม่ตรงกับ
  ลำโพงที่ชื่อ `BT Speaker` (เว้นวรรค) → เปลี่ยนค่าเริ่มต้นเป็นว่าง = ต่อตัวแรกที่เจอ
- **buffer เต็มแล้วทิ้งเสียง:** เดิมถ้า buffer ไม่พอจะรอ 500 ms แล้วทิ้งทั้งก้อน
  (ทำให้เกิดช่วงเงียบสั้น ๆ หลังทุก burst) → เปลี่ยนเป็น backpressure: หยุดอ่าน
  socket จนมีที่ว่าง (TCP กักข้อมูลไว้ให้) ไม่มีการทิ้งเสียง
- **สตรีมเริ่มตอนลำโพง connected แต่ยัง suspended:** ทำให้ buffer เต็มก่อนเล่น
  → เริ่มสตรีมเมื่อ audio state เป็น STARTED เท่านั้น

## ข้อจำกัด

- รองรับเฉพาะสตรีม **MP3** แบบตรง (audio/mpeg) — ไม่รองรับ AAC/HE-AAC, HLS (.m3u8),
  เพลย์ลิสต์ .m3u/.pls (ถ้าสถานีตอบ Content-Type อื่น จะขึ้น `reconnecting` ในหน้าเว็บ
  และ log บอก Content-Type ที่ได้รับ)
- เอาต์พุต A2DP คงที่ 44100 Hz (ข้อจำกัด SBC encoder ของ Bluedroid) — สตรีม 48 kHz
  จะถูกแปลงด้วย linear interpolation (คุณภาพเพียงพอสำหรับวิทยุ แต่ไม่ใช่ studio grade)
- Wi-Fi และ Bluetooth Classic ใช้วิทยุตัวเดียวกัน จึงมีโอกาสสะดุดมากกว่าอุปกรณ์ที่ใช้
  คลื่นเดียวอย่างเดียว; เพิ่มขนาด jitter buffer ช่วยได้
- หน้าเว็บแสดงรหัสผ่าน Wi-Fi ที่บันทึกไว้ (อยู่ในวง LAN/AP ของบอร์ดเท่านั้น)
- AVRCP รองรับ play/pause/stop/next/previous เท่านั้น (ไม่มี metadata/ชื่อเพลงขึ้นที่ลำโพง)
- สถานีที่เลือกด้วยปุ่ม Next/Previous จำไว้เฉพาะระหว่างการทำงาน — รีสตาร์ทบอร์ดจะกลับไป
  สถานีแรกในรายการ (ไม่ได้เขียน NVS ทุกครั้งที่กดเพื่อถนอม flash)

## แก้ปัญหาที่พบบ่อย

| อาการ | สาเหตุ/วิธีแก้ |
|---|---|
| ขึ้น `reconnecting` ตลอด | URL ผิด/สถานีไม่ใช่ MP3 ตรง ๆ หรือ Wi-Fi ต่อไม่ได้ — ดู log `radio` และ `Content-Type` |
| ต่อ Wi-Fi ไม่ได้ ขึ้น AP สำรอง | SSID/รหัสผิด หรือ router บล็อก — เข้า 192.168.4.1 แก้ค่า |
| Bluetooth ไม่พบลำโพง | เปิดลำโพงโหมดจับคู่, ลองเว้นชื่อลำโพงว่าง, ลองเปิด/ปิด SSP, เช็กชื่อให้ตรง |
| เสียงสะดุดเป็นช่วง ๆ | เพิ่ม jitter buffer, เปลี่ยนช่องสัญญาณ Wi-Fi ให้ห่างจาก 2.4 GHz ที่แออัด |
| เสียงช้า/lead มาก | ลด jitter buffer (200–500 ms) |
| `audio buffer init failed` | PSRAM ไม่พร้อม — ตรวจ `CONFIG_SPIRAM` และบอร์ดที่เป็น WROVER จริง |

## เอกสารที่เกี่ยวข้อง

- โปรเจกต์เดิม (I2S bridge): `../esp32cam_i2s_a2dp/README.md` และ `STATUS.md`
- ไลบรารี A2DP: `../ESP32-A2DP-main/` (Source side: `src/BluetoothA2DPSource.{h,cpp}`)
- API ที่ใช้: `esp_http_client` (IDF 5.5), `esp_crt_bundle` (HTTPS), `mp3dec.h` (Helix)
