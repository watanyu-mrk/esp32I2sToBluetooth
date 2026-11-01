# STATUS — สถานะงาน (`esp32radio_a2dp`)

อัปเดต: 1 พ.ย. 2026 · บอร์ด: **ESP32-WROVER** (PSRAM 8 MB, flash 4 MB) · พอร์ต: **COM6**
เฟิร์มแวร์: `esp32radio_a2dp` (ESP-IDF 5.5.6, target esp32)

## สรุปสถานะ

**ใช้งานได้จริงแล้ว:** บอร์ดรับสตรีม MP3 จากสถานีวิทยุผ่าน Wi-Fi → ถอดรหัส → ส่งออก
ลำโพง Bluetooth ด้วย A2DP และเริ่มดึงสตรีมทันทีที่ลำโพงเชื่อมต่อ (เพื่อให้ jitter buffer
เต็มก่อนเริ่มเล่น) — ยืนยันล่าสุด: ข้อมูลเข้า ~16 kB/s, buffer 82–99%, `underflow 0`
ตั้งค่าลิงก์สถานี / Wi-Fi / ลำโพง ผ่านหน้าเว็บได้ตลอดเวลา (ไม่ต้องแฟลชใหม่)

## รอบล่าสุด — หลายสถานี + ปุ่ม Next/Previous สลับสถานี

- ค่าตั้งเปลี่ยนจากสถานีเดียวเป็น **รายการสถานีไม่เกิน 20 รายการ** (`app_station_t`:
  url 256 + name 33) — `SETTINGS_VERSION` 2 พร้อม **migrate ค่าเดิม v1**
  (URL/ชื่อสถานีเดิมกลายเป็นรายการแรก, Wi-Fi/PIN/ลำโพงยังอยู่ครบ)
- หน้าเว็บมีรายการสถานี + ปุ่ม “＋ เพิ่มสถานี” / “ลบ” ต่อรายการ, แสดง “กำลังเล่น”
  ตาม `radio.station`, ป้องกันรายการว่าง (ปัจจุบันจำกัด 20 สถานี)
- `radio_stream`: เก็บ index สถานีปัจจุบัน, `radio_stream_next_station()` /
  `radio_stream_previous_station()` (วน modulo), เปลี่ยนสถานี = ล้าง buffer + ต่อใหม่
  ที่จุดสด; ถ้ามีสถานีเดียวจะ retune จุดสดเหมือนเดิม
- AVRCP: FORWARD → สถานีถัดไป, BACKWARD → สถานีก่อนหน้า
- `POST /api/settings` รับ `stations` array (body 3.5 KB, httpd stack 8 KB);
  `/api/state` ส่ง `settings.stations`, `radio.station`, `radio.count`

**ยืนยันบนบอร์ดจริงแล้ว (1 พ.ย. 2026) — กดปุ่มเลื่อนสถานีบนลำโพง:**

- ผู้ใช้เพิ่มสถานีเองผ่านหน้าเว็บได้ 6 รายการ (`COOLfahrenheit`, `90`, `cill`, `red`,
  `top`, `lay`) — `POST /api/settings` เก็บ/คืนรายการได้ครบหลังรีบูต
- กดปุ่มย้อนกลับบนลำโพง 6 ครั้ง: `key_code 0x4c` → เลื่อนสถานี
  **1/6 → 6/6 → 5/6 → 4/6 → 3/6 → 2/6 → 1/6** (จากสถานีแรกกดย้อนกลับ = วนไปสถานี
  สุดท้าย) ทุกครั้งขึ้น `radio: station n/6 'ชื่อ'` แล้วต่อสตรีมใหม่และขึ้น
  `radio playing <kbps> 44100 Hz 2ch`
- `drop 0` ตลอดการทดสอบ (buf 62–100%), decode err เพิ่มครั้งละ 1–2 ต่อการเปลี่ยนสถานี
- ข้อสังเกต: สถานี `top` (`a10.asurahosting.com:8250`, 22050 Hz mono) ทำให้ decode err
  กระโดด ~1620 ครั้งตอนเปิด แต่ยังเล่นได้ (143 → 96 kbps)
- **ยังไม่ได้ยืนยันปุ่ม Next (0x4b FORWARD)** — รอบนี้มีแต่ 0x4c เข้ามาใน log;
  โค้ดเส้นทางเดียวกัน (`switch_station(+1)`)

## รอบล่าสุด (1 พ.ย. 2026) — ระดับเสียงตอนเริ่มเล่น

- ค่าตั้งใหม่ `volume` (0–127) เก็บใน NVS, `SETTINGS_VERSION` 2 → **3** พร้อม
  **migrate อัตโนมัติ** (รายการสถานี/Wi-Fi/ลำโพง/PIN คงอยู่, volume ใช้ค่าโรงงาน)
- ค่าโรงงาน `CONFIG_APP_INITIAL_VOLUME` = **60**, หน้าเว็บมีสไลเดอร์
  “ระดับเสียงตอนเริ่มเล่น (n/127)”, `/api/state` ส่ง `settings.volume`,
  `POST /api/settings` รับ `volume` (เกิน 127 ถูก clamp เป็น 127)
- `a2dp_source.cpp`: ตอน audio state เป็น `STARTED` จะเรียก `set_volume()` เพื่อ
  **ส่ง AVRCP absolute volume ให้ลำโพง** (ลำโพงจำระดับเสียงเองไม่ได้ จึงต้องส่ง
  ทุกครั้งที่เริ่มเล่น; กันการยิงซ้ำด้วย debounce 2 s และกัน `set_volume()` ที่
  ไลบรารีข้ามเมื่อค่าไม่เปลี่ยน)
- ยืนยันบนบอร์ด: `settings: migrated from the version 2 layout ... volume 60/127`,
  `a2dp_src: startup volume n/127 pushed to the speaker`,
  `RCCT: Set absolute volume response: volume n` — และ API: `{"volume":30}`
  รีบูตแล้วได้ 30, `{"volume":200}` → 127

## รอบล่าสุด (1 พ.ย. 2026) — ขยายเป็น 20 สถานี

- `APP_SETTINGS_MAX_STATIONS` 6 → **20**; `SETTINGS_VERSION` 3 → **4** พร้อม migrate
  จากรูปแบบ 6 สถานีเดิม (v2/v3) — รายการสถานี/Wi-Fi/ลำโพง/PIN/ระดับเสียงอยู่ครบ
- บลอบค่าตั้งโตเป็น ~6 KB จึงย้ายบัฟเฟอร์ชั่วคราวไป **PSRAM**: บลอบที่เขียนลง NVS,
  สำเนาสำหรับ validate และ body ของ POST (`SETTINGS_BODY_MAX` ≈ 7.5 KB);
  `s_settings` ตัวจริงยังอยู่ใน internal RAM
- `/api/state` ส่ง `max_stations` และหน้าเว็บใช้ค่านั้นเป็นลิมิตของปุ่มเพิ่มสถานี
- **บักที่เจอตอนทดสอบ:** `app_settings_t next` เป็นตัวแปรบน stack ของ task httpd
  (8192 B) — พอ struct โตเป็น ~6 KB task ตายตั้งแต่ POST แรก แล้วทุก request
  หลังจากนั้น timeout (บอร์ดยังเล่นเพลงต่อ) → ย้ายไป PSRAM แล้ว
- ยืนยันบนบอร์ด: บันทึก 20 สถานี (body 1443 B) ได้ `{"ok":true,"reboot":true}` ใน ~3 s,
  รีบูตแล้วได้ครบ 20 รายการ, คืนรายการเดิมได้, หลังรีบูตกลับมา `radio playing` + `drop 0`

## รอบล่าสุด (1 พ.ย. 2026) — เสียงกระตุกหนัก: read() คืน 0 = EAGAIN ไม่ใช่จบสตรีม

**อาการ:** หลังอัปเดต เสียงขาด ๆ หาย ๆ เกือบทั้งเพลง (log บอร์ดขึ้น `radio reconnecting`
เป็นรอบ ๆ ทุก ~15 s) วัดได้ข้อมูลจากสถานีเพียง **755 B/s** จากที่ต้องใช้ 16,000 B/s,
buffer `0% (0/65536 fr)` และ `underflow` ~13,000–40,000 เฟรม/s (เงียบ ~75–100% ของเวลา)
ทั้งที่ Wi-Fi `-57 dBm`, heap เหลือ ~28 KB, `l2cab is_cong` = 0

**สาเหตุ:** `esp_http_client_read()` บนซ็อกเก็ต non-blocking ของ `esp_http_client`
คืน **0** (หรือ `ERROR` เมื่อ errno = 11 = `EAGAIN`) ในจังหวะที่ TLS record ยังมาไม่ครบ
ไม่ใช่สัญญาณว่าสถานีปิดสตรีม แต่ loop เดิมถือว่า 0 = จบสตรีม:

```c
if (read == 0) { ESP_LOGW(TAG, "station closed the stream"); ok = false; break; }
```

⇒ ทุกครั้งที่ข้อมูลสะดุดเสี้ยววินาที บอร์ดตัด connection ทิ้ง → ต่อใหม่ → เจอ EAGAIN อีก
→ วนลูป reconnect ที่ไม่มีข้อมูลเข้าบัฟเฟอร์เลย หลักฐานใน log คือคู่
`transport_base: esp_tls_conn_read error, errno=No more processes` +
`HTTP_CLIENT: esp_transport_read returned:0 and errno:11` แล้วตามด้วย reconnect

**แก้ที่ `main/radio_stream.c` (stream loop):**

- `read > 0` → นับ `s_info.bytes`, รีเซ็ต idle timer, ถอดรหัสต่อ
- `read == 0` หรือ error → **ไม่ตัดสายทันที**: `vTaskDelay(20 ms)` แล้วลองอ่านต่อ
  (หยุดพักให้ task Wi-Fi/lwIP/Bluetooth ได้รัน ซึ่งเป็นผู้บริโภคซ็อกเก็ตตัวเดียวกัน)
- ตัดสินว่าสตรีมตายเมื่อ **ไม่มีไบต์เข้าเลยติดต่อกัน 10,000 ms** → log
  `no data from the station for 10010 ms, reconnecting` แล้วค่อยต่อใหม่

**ทดลองประกอบ (ระหว่างหาสาเหตุ):** ขยาย `CONFIG_LWIP_TCP_WND_DEFAULT` 5760 → 23400
ไม่ช่วย (ไม่ใช่คอขวด window/RTT) จึง**คืนค่า 5760** ตามเดิมเพื่อประหยัด RAM

**ผลวัดหลังแฟลช (COM6, เก็บ log 90 s ระหว่างเล่นจริง):**

| ตัวชี้วัด | ก่อนแก้ | หลังแก้ |
|---|---|---|
| ข้อมูลจากสถานี | 755 B/s | **16,000–16,900 B/s** |
| jitter buffer | `0% (0/65536 fr)` | **82–99%** |
| underflow / ความเงียบ | ~44,000 เฟรม/s (เงียบ 100%) | **0 เฟรม ใน ~70 s** |
| `read failed` / reconnect | ทุก ~15 s | **0** |
| BT congestion (`l2cab is_cong`) | เป็นรอบ ๆ | **0** |
| heap (`MALLOC_CAP_INTERNAL`) | 27–28 KB | ~28 KB คงที |

**แก้ควบคู่ในรอบเดียวกัน (คนละเรื่องกับ EAGAIN แต่ช่วยเรื่องเดียว):**

- `main.cpp`: เปลี่ยนเงื่อนไขเปิดสตรีมจาก `a2dp_source_is_streaming()` เป็น
  `a2dp_source_is_connected()` — เริ่มดึงข้อมูลตั้งแต่ลำโพงเชื่อมต่อ บัฟเฟอร์จึงเต็มก่อน
  ที่ PCM ก้อนแรกจะถูกขอ (เดิมบัฟเฟอร์ว่างตอนเริ่ม → BT ยิง silence รัว → แย่งวิทยุกับ Wi-Fi)
- `main/a2dp_source.cpp`: เลิกใช้ `set_volume()` ของไลบรารีในเส้นทางเสียง (มันเปิด
  software volume scaling ที่ไล่คูณทุกเฟรมบน task Bluetooth) แล้วส่ง AVRCP absolute volume
  ผ่าน `esp_avrc_ct_send_set_absolute_volume_cmd()` ตรง ๆ แทน

**ข้อสังเกต:** การวัดรอบที่ลำโพงหลุด (`BT Disconnected` / `radio idle (no speaker)` หลังแฟลช)
ตัวเลขจะเพี้ยนเป็น 0 — ต้องรอ `BT Streaming` ก่อนวัด; รอบนี้ระบบต่อกลับเองได้ในไม่กี่วินาที

## ยืนยันบนฮาร์ดแวร์แล้ว

| หัวข้อ | หลักฐานจาก Serial (COM6) |
|---|---|
| บูต + PSRAM | `PSRAM 8388608 bytes`, `buffer: 65536 frames (1486 ms)` |
| Config page | `portal: config page ready on port 80` |
| Wi-Fi STA + DHCP | `connected with spd_wifi`, `Wi-Fi connected 192.168.8.67 (-56 dBm)` |
| Setup AP สำรอง | `setup access point 'ESP32-Radio-Setup' up` แล้ว `station is up: setup access point closed` |
| Bluetooth A2DP | ค้นเจอ `BT Speaker` → `Target device found` → `a2dp_src: audio state: started` |
| สตรีม HTTPS/TLS | `radio: stream open`, `radio playing 128 kbps 44100 Hz 2ch`, `decode err 2` (เฉพาะช่วงต่อ mid-frame) |
| เสียงออกลำโพง | **ผู้ใช้ยืนยันว่าได้ยินเสียง** |
| AVRCP Target | `A2DP Enable with AVRC`, `For AVRCP Target Category 1 and 3...`, TG callback ถูกเรียก (`Unsupported AVRC TG event: 6` = PROF_STATE, ไม่มีผล) |
| ปุ่มบนลำโพงเลื่อนสถานี | `AVRC passthrough cmd: key_code 0x4c` ×6 → `station 1/6 → 6/6 → 5/6 → 4/6 → 3/6 → 2/6 → 1/6` แต่ละครั้งขึ้น `radio playing ... 44100 Hz 2ch` |
| `drop 0` ตอนเล่นจริง (หลังแก้ backpressure) | log สถานะทุก 5 s: `buf 62–100% (40640–65536/65536 fr) | drop 0` ตลอด 5 นาทีที่ทดสอบ |

ค่าที่ใช้ทดสอบ: SSID `spd_wifi`, sink `BT Speaker` (00:58:56:74:31:f3), PIN `1234`,
SSP ปิด, buffer 1000 ms (จริง 65536 เฟรม ≈ 1.49 s), IP 192.168.8.67

## แก้ไขระหว่างทดสอบบนบอร์ด (รอบนี้)

1. **Wi-Fi ไม่เริ่มหลังรีบูต** — `esp_wifi_init()` ทิ้งโหมดเป็น `WIFI_MODE_NULL` จึงต้อง
   `esp_wifi_set_mode(WIFI_MODE_STA)` ก่อน `esp_wifi_set_config()` (เดิมได้ `ESP_ERR_WIFI_MODE`)
2. **ไม่ได้ IP สักที** — `wifi_link_poll()` เรียก `esp_wifi_connect()` ซ้ำทุก 3 s ขณะรอ DHCP
   (log: `disconnect before connecting to new ap`) → retry เฉพาะเมื่อ station หลุดจริง (`s_sta_up`)
3. **AP สำรองเปิดทุกบูต** — timeout หน่วยผิด (25 s กลายเป็น 25 ms) → แก้เป็น µs และ
   **ปิด AP อัตโนมัติเมื่อได้ IP** (คืนเวลา radio ให้ Bluetooth/Wi-Fi coex)
4. **เสียงหายเป็นช่วง ๆ** — buffer เต็มแล้วทิ้งทั้งก้อน (log `drop` > 1.1M เฟรม) → เปลี่ยนเป็น
   backpressure (หยุดอ่าน socket จนมีที่ว่าง, TCP กักข้อมูลไว้) + เริ่มสตรีมเมื่อ audio state
   เป็น `STARTED` (เดิมเริ่มตอน connected แต่ยัง suspended ทำให้ buffer เต็มก่อนเล่น)
5. **ชื่อลำโพงเริ่มต้นไม่ตรง** — `BT-Speaker` ไม่แมตช์ `BT Speaker` → ค่าเริ่มต้นเป็นว่าง
   (= ต่อตัวแรกที่เจอ) แล้ว erase NVS + แฟลชใหม่

## AVRCP (เพิ่มตามคำขอ)

ลงทะเบียนเป็น AVRCP Target ด้วย `set_avrc_passthru_command_callback()` (เป็นตัวที่ทำให้
ไลบรารี init TG ด้วย) — ไม่ต้องใช้ component เพิ่ม

| ปุ่มบนลำโพง | การทำงาน |
|---|---|
| Play | เล่นต่อ / ต่อสตรีมใหม่ที่จุดสด |
| Pause, Stop | หยุดชั่วคราว ปิด socket, สถานะ `paused (remote)` |
| Next, Previous | เลื่อนไปสถานีถัดไป / ก่อนหน้า (วนกลับหัว–ท้ายรายการ) |
| อื่น ๆ | เขียน log แล้วไม่ทำอะไร |

ไฟล์: `main/a2dp_source.cpp` (`on_avrc_passthrough`), `main/radio_stream.{h,c}`
(`radio_stream_set_paused()`, `radio_stream_restart()`)

## ยังไม่ได้ทดสอบ / ค้างอยู่

- **ปุ่ม Next (0x4b) และ Play/Pause บนลำโพง** — รอบทดสอบ 1 พ.ย. มีแต่ปุ่มย้อนกลับ
  (`key_code 0x4c`) เข้ามา; Next/Play/Pause ยังไม่ได้กดตรวจ (เส้นทางโค้ดเดียวกัน)
- Reconnect เมื่อลำโพงหลุด/ปิดกลางเพลง (โค้ดมีอยู่ ยังไม่ทดสอบกับฮาร์ดแวร์)
  — หลังรีบูตหนึ่งครั้งรอบนี้ลำโพงไม่ต่อกลับเองจนกว่าจะรีบูต/เปิดลำโพงใหม่
- สตรีม 48 kHz (เส้นทาง resampler) และสถานีที่ตอบ Content-Type อื่น
- เล่นต่อเนื่องยาว (ยังไม่ทดสอบ 30 นาที+)

## ข้อจำกัดที่รู้

- **ถ้าเชื่อมต่อครั้งแรกล้มเหลว** (เช่น มือถือจับลำโพงอยู่) ไลบรารี ESP32-A2DP จะอยู่นิ่ง
  ไม่ลองใหม่ (heartbeat เห็น `Auto-reconnect disabled`) → ต้องรีสตาร์ทบอร์ดหรือเปิดลำโพงให้ว่าง
  แพตช์ได้ที่ `bt_app_av_state_unconnected_hdlr` (fallback ไป discovery) แต่จะกระทบโปรเจกต์
  เดิมที่ใช้ component ร่วมกัน
- รองรับเฉพาะ MP3 ตรง ๆ (ไม่มี AAC/HLS/playlist)
- เอาต์พุต A2DP คงที่ 44100 Hz (SBC ของ Bluedroid) — สตรีมอื่นถูก resample แบบ linear
- Wi-Fi + Bluetooth Classic ใช้วิทยุร่วมกัน จึงมีโอกาสสะดุด; buffer ใหญ่ช่วยได้
- หน้าเว็บแสดงรหัสผ่าน Wi-Fi ที่บันทึกไว้ (เฉพาะในวง LAN/AP ของบอร์ด)

## ขั้นตอนทดสอบต่อ

1. เปิดลำโพง (ปลดจากมือถือถ้าจำเป็น) แล้วรอ `BT Streaming` ใน log
2. กด Pause → ควรเห็น `radio: paused from the remote control`, `radio stopped`,
   หน้าเว็บขึ้น `paused (remote)`
3. กด Play → `radio: resumed from the remote control` แล้ว `stream open` ใหม่
4. กดปุ่ม Next (FORWARD) → ควรเห็น `AVRC passthrough cmd: key_code 0x4b` ตามด้วย
   `radio: station (n+1)/6: ...`, ปุ่มย้อนกลับ (0x4c) ยืนยันแล้วว่าใช้ได้
5. ระหว่างเล่น ดูบรรทัดสถานะทุก 5 s: ต้องเห็น `radio playing 128 kbps 44100 Hz 2ch`
   และ `drop 0` (ถ้า drop โตอีก แปลว่า backpressure ยังไม่พอ)

## Build / Flash (เครื่องนี้)

```bash
export IDF_PATH=/c/esp/esp-idf
export IDF_TOOLS_PATH=/c/esp/espressif
export PATH="/c/esp/espressif/tools/xtensa-esp-elf/esp-14.2.0_20260121/xtensa-esp-elf/bin:\
/c/esp/espressif/tools/cmake/3.30.2/bin:/c/esp/espressif/tools/ninja/1.12.1:\
/c/esp/espressif/python_env/idf5.5_py3.12_env/Scripts:$PATH"
cd esp32radio_a2dp
ninja -C build                       # หรือ idf.py build จาก cmd.exe
# แฟลชเฉพาะ app (เก็บ NVS/Wi-Fi ที่ตั้งไว้):
#   esptool --chip esp32 -p COM6 -b 460800 write_flash \
#     --flash_mode dio --flash_freq 40m --flash_size 4MB 0x10000 build/esp32radio_a2dp.bin
```

> บนเครื่องนี้ `idf.py` ใช้ไม่ได้จาก Git Bash (IDF 5.5 ปฏิเสธ MSys) จึงใช้ `cmake` + `ninja`
> ตรง ๆ ตาม README; ถ้า build จาก cmd.exe ให้ใช้ `export.bat` + `idf.py build`
