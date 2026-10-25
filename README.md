# ESP32 I2S → Bluetooth A2DP Bridge

ส่งเสียงจาก Internet Radio ที่เล่นบน **ESP32-S3** ไปออก **ลำโพง Bluetooth** โดยมี
**ESP32-CAM (ESP32 รุ่นแรก)** ทำหน้าที่เป็นตัวรับ I2S แล้วส่งต่อด้วย Bluetooth Classic
A2DP Source — ลำโพงรุ่นไหนก็ได้ที่รองรับ A2DP (ส่วนใหญ่คือลำโพง/หูฟัง Bluetooth ทั่วไป)

```
Internet Radio / MP3 (ESP32-S3)
      │  I2S — S3 เป็น Master สร้าง BCLK/WS ให้ฝ่ายเดียว
      ▼
ESP32-CAM ── I2S Slave RX ──► แปลง 32-bit slot → PCM16 stereo
      │                        └► ring buffer (SPSC, lock-free)
      ▼
Bluetooth A2DP Source ──► ลำโพง Bluetooth

(ทางแยกเดิมยังทำงานอยู่: S3 → PCM5102A ของเครื่องวิทยุ ไม่ถูกแก้ไข)
```

## สารบัญ

- [คุณสมบัติ](#คุณสมบัติ)
- [โครงสร้าง repository](#โครงสร้าง-repository)
- [ฮาร์ดแวร์และผังสาย](#ฮาร์ดแวร์และผังสาย)
- [ลำดับการทำงานตอนบูต](#ลำดับการทำงานตอนบูต)
- [การตั้งค่า](#การตั้งค่า)
  - [1) หน้าเว็บผ่าน Wi-Fi (แก้ได้โดยไม่ต้อง build ใหม่)](#1-หน้าเว็บผ่าน-wi-fi-แก้ได้โดยไม่ต้อง-build-ใหม่)
  - [2) menuconfig (ค่าโรงงาน / ค่าที่แก้จากเว็บไม่ได้)](#2-menuconfig-ค่าโรงงาน--ค่าที่แก้จากเว็บไม่ได้)
  - [3) HTTP API](#3-http-api)
- [Build / Flash / Monitor](#build--flash--monitor)
- [การใช้งานจริง](#การใช้งานจริง)
- [ทดสอบแยกส่วน](#ทดสอบแยกส่วน)
- [แก้ปัญหาที่พบบ่อย](#แก้ปัญหาที่พบบ่อย)
- [ข้อจำกัดและสิ่งที่ยังไม่ได้ยืนยัน](#ข้อจำกัดและสิ่งที่ยังไม่ได้ยืนยัน)
- [เอกสารเพิ่มเติม](#เอกสารเพิ่มเติม)

## คุณสมบัติ

| ด้าน | รายละเอียด |
|---|---|
| บทบาท Bluetooth | **A2DP Source** (ส่งเสียง) + AVRCP — ไม่ใช่ sink/receiver |
| รับสัญญาณเสียง | I2S **slave** 32-bit slot, แปลงเป็น PCM16 stereo (shift 16) |
| Sample rate | A2DP ออก **44100 Hz คงที่** (SBC ของ Bluedroid ฮาร์ดโค้ดไว้) — ฝั่งรับเลือก 44.1k/48k ได้ และจะ resample แบบ linear interpolation ให้อัตโนมัติ |
| Jitter buffer | 50–2000 ms (ปัดขึ้นเป็น power of two, อยู่ใน internal RAM) |
| ค้นหาลำโพง | discovery อัตโนมัติ + auto-reconnect (1000 ครั้ง) + ล้าง peer เก่าใน NVS ตอนบูตเมื่อระบุชื่อลำโพง |
| ตั้งค่า | หน้าเว็บผ่าน AP `ESP32-A2DP-Setup` เก็บใน NVS (namespace `appcfg`) |
| Wi-Fi | เปิดเฉพาะ **โหมดตั้งค่า** และเปิดหลังปิด Bluetooth แล้วเท่านั้น (ดูด้านล่าง) |
| Self-test | loopback I2S + resampler, โทน 1 kHz, ตรวจว่า BCLK/WS/DATA มีสัญญาณตอนบูต |

## โครงสร้าง repository

```
.
├── README.md                  # ไฟล์นี้ — ภาพรวม + วิธีตั้งค่า/ใช้งานทั้งหมด
├── promp.txt                  # สเปกโปรเจกต์ (ภาษาไทย)
├── knowledge.md               # โน้ตความรู้/library + แผนที่โค้ด
├── ESP32-A2DP-main/           # ไลบรารี ESP32-A2DP (vendored, Apache-2.0) + docs
└── esp32cam_i2s_a2dp/         # โปรเจกต์ ESP-IDF 5.5 (target esp32) — ตัว firmware
    ├── README.md              # รายละเอียดเชิงลึก (ผังสาย/BOM/ผลทดสอบ/ทุกหัวข้อ)
    ├── STATUS.md              # handoff: ทำอะไรแล้ว, ยืนยันบนบอร์ดแล้วหรือยัง, ขั้นต่อไป
    ├── partitions.csv         # app 3 MB (image จริง ~1.4 MB)
    ├── sdkconfig.defaults     # BT Classic + A2DP, Wi-Fi buffer สำหรับ portal
    ├── main/                  # โค้ดทั้งหมด (ดูตารางด้านล่าง)
    └── components/            # dependency ภายนอก (ไม่ commit — ดูหัวข้อติดตั้ง)
```

โค้ดใน `main/`:

| ไฟล์ | หน้าที่ |
|---|---|
| `main.cpp` | `app_main`: boot info, สถานะทุก 5 s, ตัดสินใจเปิด config portal |
| `i2s_receiver.c` | task รับ I2S (slave) + แปลง 32-bit slot → PCM16 |
| `audio_buffer.c` | SPSC ring buffer แบบ atomic (ไม่ใช้ mutex) |
| `resampler.c` | 48k → 44.1k (linear interpolation) |
| `a2dp_source.cpp` | `BluetoothA2DPSource` + data callback + ปิด stack (`end(true)`) |
| `app_settings.c` | อ่าน/เขียนค่าตั้งค่าใน NVS + ค่าโรงงานจาก Kconfig |
| `config_portal.c` | Wi-Fi AP + HTTP server + API |
| `main/web/index.html` | หน้าเว็บ (ฝังใน firmware ด้วย `EMBED_FILES`) |
| `bt_scan.c` | แคชรายชื่อลำโพงที่ discovery เจอ (8 ตัว) |
| `pin_activity.c` | ตรวจ BCLK/WS/DATA ว่ามีสัญญาณก่อนเริ่ม I2S |
| `tone_source.c` | โทน 1 kHz แทน I2S (ทดสอบ Bluetooth เดี่ยว ๆ) |
| `loopback_test.c` | self-test เส้นทางรับข้อมูล + resampler |

## ฮาร์ดแวร์และผังสาย

| ฝั่ง ESP32-S3 (วิทยุ/PCM5102A เดิม) | ฝั่ง ESP32-CAM | หน้าที่ |
|---|---|---|
| GPIO1 (BCLK) | GPIO14 | I2S Bit Clock |
| GPIO2 (LRCK/WS) | GPIO13 | I2S Word Select |
| GPIO40 (DATA) | GPIO4 | I2S Data |
| GND | GND | **ต้องต่อร่วม** ไม่งั้นมีเสียงรบกวน/เงียบ |

บอร์ดที่ใช้ทดสอบ: ESP32-D0WD rev1.0, flash 4 MB (DIO/40 MHz), 2 cores/160 MHz,
BT MAC `30:ae:a4:d8:41:86`, พอร์ต `COM6`, ESP-IDF v5.5.6

ข้อควรระวัง:

- **S3 ต้องเป็น master เพียงเจ้าเดียว** — อย่าให้ CAM สร้าง BCLK/WS
- GPIO4 ต่อกับวงจร Flash LED ของ CAM ⇒ LED อาจวูบวาบและกวนเส้น DATA;
  ถ้าเสียงแตกให้ย้าย DATA ไป GPIO13/15/2 (ขา SD ที่ไม่ใช้) แล้วแก้ใน menuconfig
- GPIO13/14/4 เป็นขา microSD — โปรเจกต์นี้ไม่ใช้ SD จึงใช้ได้
- ห้ามใช้ GPIO16/17 (PSRAM) และเลี่ยง GPIO0/2/12/15 (strapping), ห้ามป้อน 5 V เข้า GPIO

## ลำดับการทำงานตอนบูต

```
boot
 ├─ อ่านค่าจาก NVS (ถ้าไม่มีให้ใช้ค่า Kconfig) → จอง ring buffer → ตรวจสัญญาณ BCLK/WS/DATA
 ├─ เริ่ม Bluetooth: discovery + พยายามต่อลำโพงตามชื่อที่ตั้งไว้
 │    ✔ ต่อได้ → stream ไปเรื่อย ๆ (Wi-Fi ไม่ถูกเปิดเลย)
 │    ✘ ไม่ได้ภายใน CONFIG_APP_PORTAL_DELAY_S (ค่าเริ่มต้น 180 s)
 │         ├─ ปิด Bluetooth ทั้งก้อน: end(true) → Bluedroid/controller deinit
 │         │  + คืนหน่วยความจำ Classic BT ให้ระบบ (~126 KB ในบอร์ดทดสอบ)
 │         ├─ คืน ring buffer ทั้งก้อน (โหมดตั้งค่าไม่มีเสียงอยู่แล้ว)
 │         └─ เปิด AP `ESP32-A2DP-Setup` + HTTP server (~146 KB free)
 └─ โหมดตั้งค่า: เปิดค้างจนกด “บันทึกและรีสตาร์ท” หรือรีสตาร์ทบอร์ดเอง
```

เหตุผลที่ออกแบบแบบนี้: Wi-Fi กับ Bluetooth Classic ใช้ **วิทยุและ heap ก้อนเดียวกัน**
การเปิด AP ตั้งแต่ต้นจะแย่งทรัพยากรกับ A2DP (บิลด์เก่าเปิด AP หลังบูต 3 วินาทีและมี
ปัญหาลำโพงต่อไม่ติด/เสียงหลุด) จึงให้ Bluetooth ได้ทุกอย่างก่อน แล้วค่อยยอมสละมัน
เมื่อหาลำโพงไม่ได้จริง ๆ

> **เปิด Bluetooth กลับไม่ได้ในรอบบูตเดียวกัน** เพราะหน่วยความจำของ controller
> ถูกคืนให้ระบบแล้ว (`esp_bt_controller_mem_release`) — ต้องรีสตาร์ท ซึ่งคือสิ่งที่
> ปุ่ม “บันทึกและรีสตาร์ท” ทำให้นั่นเอง

## การตั้งค่า

### 1) หน้าเว็บผ่าน Wi-Fi (แก้ได้โดยไม่ต้อง build ใหม่)

```bash
# ต่อ Wi-Fi (โน้ต: ถ้า Windows เคยจำ profile ผิด ให้ลบแล้วต่อใหม่)
SSID     : ESP32-A2DP-Setup
Password : esp32cam
URL      : http://192.168.4.1/
```

เปิดได้เมื่อ **บอร์ดหาลำโพงไม่เจอครบ 3 นาทีแล้วเท่านั้น** (ดู log
`app: no speaker after 180 s: switching to the config portal`)

| ตั้งได้จากหน้าเว็บ | ค่า | หมายเหตุ |
|---|---|---|
| ชื่อลำโพงเป้าหมาย | 0–32 ตัวอักษร | เทียบแบบ **ขึ้นต้นด้วย**; เว้นว่าง = ต่อกับ sink ตัวแรกที่เจอ |
| ชื่อ Bluetooth ของบอร์ด | 0–32 ตัวอักษร | ชื่อที่ประกาศออกไป |
| Secure Simple Pairing (SSP) | เปิด/ปิด | เปิดสำหรับลำโพงรุ่นใหม่ที่เลิกใช้ PIN แบบ legacy |
| I2S input rate | 44100 / 48000 Hz | ต้องตรงกับที่ S3 ส่งมา; ไม่ตรงจะ resample ให้ |
| ขนาด jitter buffer | 50–2000 ms | มากขึ้น = ทนกระตุกได้นานขึ้น แต่ latency สูงขึ้นและกิน RAM มากขึ้น |

พฤติกรรมที่ควรรู้:

- รายการลำโพงที่เห็นมาจาก discovery **ช่วง 3 นาทีก่อนหน้า** — ตอนหน้าเว็บเปิด
  Bluetooth ถูกปิดแล้ว จึงกด “ค้นหาใหม่” ไม่ได้ (ปุ่มจะถูก disable) ให้พิมพ์ชื่อเอง
- **A2DP ส่งออก 44100 Hz คงที่** หน้าเว็บจึงแสดงแบบอ่านอย่างเดียว (SBC encoder ของ
  Bluedroid ฮาร์ดโค้ด `pcm.sampling_freq = 44100`; ถ้าฝืนตั้ง 48000 เสียงจะเร็วขึ้น ~9%)
- กด “บันทึกและรีสตาร์ท” → เขียน NVS → ตอบ `{"ok":true,"reboot":true}` → รีบูตใน
  ~1.2 วินาที → กลับไปโหมด Bluetooth (AP หายไป)
- ค่าที่บันทึกจะถูกตรวจช่วงและถอยไปใช้ค่าเดิมทีละฟิลด์ถ้าไม่ถูกต้อง

### 2) menuconfig (ค่าโรงงาน / ค่าที่แก้จากเว็บไม่ได้)

`idf.py menuconfig` → **ESP32-CAM I2S to A2DP bridge**

| ตัวเลือก | ค่าเริ่มต้น | ความหมาย |
|---|---|---|
| Local Bluetooth name | `ESP32-CAM-A2DP` | ค่าโรงงานของชื่อบอร์ด |
| Bluetooth speaker name | `BT-Speaker` | ค่าโรงงานของชื่อลำโพง |
| Use Secure Simple Pairing | ปิด | ค่าโรงงานของ SSP |
| A2DP sample rate | 44100 | ใช้ 44100 เท่านั้นในทางปฏิบัติ |
| I2S input sample rate | 44100 | ตรงกับที่ S3 ส่ง |
| I2S BCLK / WS / DATA GPIO | 14 / 13 / 4 | ตามผังสาย |
| PCM shift bits | 16 | 16 = PCM16 อยู่ครึ่งบนของ slot 32-bit |
| Audio buffer | 300 ms | จริงได้ 16384 frames = 371 ms (ปัดเป็น power of two) |
| Check pin activity at boot | เปิด | log ว่า BCLK/WS/DATA toggling หรือ static |
| Pin check window | 60 ms | ระยะเวลาสุ่มดูสัญญาณ |
| Loopback self test | ปิด | เปิดเพื่อทดสอบเส้นทางรับข้อมูลตอนบูต |
| Test tone | ปิด | โทน 1 kHz แทน I2S |
| **Portal delay** | **180 s** | เวลารอต่อลำโพงก่อนปิด Bluetooth แล้วเปิด AP |
| Status log interval | 5000 ms | ความถี่ของบรรทัด `BT=... heap ...` |

ไฟล์ `sdkconfig.defaults` ตรึงค่าที่สำคัญไว้แล้ว: target `esp32`, BT Classic +
A2DP (ไม่เปิด BLE), Wi-Fi ปิด IRAM optimization (ไม่งั้น `iram0_0_seg` ล้น),
Wi-Fi buffer เล็กลง (`STATIC_RX_BUFFER_NUM=2`, `DYNAMIC_*=8`), `LWIP_MAX_SOCKETS=8`,
`TCP_WND=2880`, `FREERTOS_HZ=1000`, partition table แบบกำหนดเอง

### 3) HTTP API

| Method | Path | Body / ผลลัพธ์ |
|---|---|---|
| GET | `/` | หน้าเว็บ (ฝังใน firmware) |
| GET | `/api/state` | `{bt, connected, scanning, bt_off, settings{target, local, ssp, input_rate, output_rate, buffer_ms}, devices[{name, addr, rssi}]}` |
| POST | `/api/settings` | `{target, local, ssp, input_rate, buffer_ms}` → `{"ok":true,"reboot":true}` (บันทึก NVS + รีสตาร์ท 1.2 s) |
| POST | `/api/rescan` | `{"ok":true}` — ล้างรายการ (ถูกข้ามถ้า Bluetooth ปิดอยู่แล้ว) |
| POST | `/api/reboot` | `{"ok":true,"reboot":true}` |

ตัวอย่าง:

```bash
curl http://192.168.4.1/api/state
curl -X POST http://192.168.4.1/api/settings \
  -H 'Content-Type: application/json' \
  -d '{"target":"BT Speaker","local":"ESP32-CAM-A2DP","ssp":false,"input_rate":44100,"buffer_ms":300}'
```

## Build / Flash / Monitor

ต้องมี ESP-IDF **v5.5** (ทดสอบด้วย 5.5.6) และ dependency ภายนอกที่ **ไม่ commit**:

```bash
cd esp32cam_i2s_a2dp
mkdir -p components

# 1) ไลบรารี A2DP (คัดลอกจากโฟลเดอร์ vendored ใน repo นี้ หรือ clone เอง)
cp -r ../ESP32-A2DP-main components/ESP32-A2DP

# 2) AudioTools — บังคับโดย CMakeLists ของ ESP32-A2DP
git clone --depth 1 https://github.com/pschatzmann/arduino-audio-tools.git \
    components/arduino-audio-tools
```

build และแฟลช (port ตัวอย่าง = `COM6`):

```bash
idf.py set-target esp32        # ครั้งแรกครั้งเดียว
idf.py build
idf.py -p COM6 flash monitor
```

> บน Windows เครื่องนี้ ESP-IDF อยู่ที่ `C:\esp\esp-idf` และ `idf.py` ตรง ๆ จะพัง
> ถ้าเรียกจาก Git Bash (ตัวแปร `MSYSTEM`) — ใช้ wrapper ที่เตรียมไว้:
> `source /c/esp/idf-env.sh` แล้ว
> `.../python.exe /c/esp/run_idf.py -p COM6 flash monitor`
> หรือเปิด **ESP-IDF 5.5 CMD/PowerShell** ตามปกติ

### ไฟล์ image เดียว (`firmware.bin`)

`firmware.bin` คือ image ที่รวม bootloader + partition table + application ไว้ไฟล์เดียว
(สร้างใหม่ทุก build, อยู่ใน `.gitignore`) เหมาะกับการแฟลชบอร์ดเปล่า:

```bash
cd build
esptool.py --chip esp32 merge_bin -o ../firmware.bin \
  --flash_mode dio --flash_freq 40m --flash_size 4MB \
  0x1000 bootloader/bootloader.bin \
  0x8000 partition_table/partition-table.bin \
  0x10000 esp32cam_i2s_a2dp.bin

cd ..
esptool.py --chip esp32 -p COM6 -b 460800 write_flash 0x0 firmware.bin
```

| ไฟล์ | Offset | หมายเหตุ |
|---|---|---|
| `firmware.bin` (merged) | `0x0` | เติมช่วงว่างด้วย `0xFF` ⇒ **NVS/ค่าตั้งค่า/คู่ BT หายทั้งหมด** |
| `build/esp32cam_i2s_a2dp.bin` | `0x10000` | เก็บค่าที่ตั้งไว้เดิม (แนะนำเมื่ออัปเดตเวอร์ชัน) |
| `build/bootloader/bootloader.bin` | `0x1000` | |
| `build/partition_table/partition-table.bin` | `0x8000` | app 3 MB |

## การใช้งานจริง

1. ต่อสาย S3 → CAM ตามผัง (PCM5102A ยังต่ออยู่ได้ตามปกติ)
2. แฟลช firmware แล้วเปิดบอร์ด — log ตอนบูตที่ควรเห็น:

   ```
   settings: loaded: sink 'BT Speaker', local 'ESP32-CAM-A2DP', SSP off, I2S 48000 Hz, A2DP 44100 Hz, buffer 300 ms
   app: audio buffer: 16384 frames (371 ms)
   app: waiting for a speaker (config portal opens after 180 s)
   i2s_rx: slave RX: BCLK=14 WS=13 DIN=4, 32-bit slots, shift=16, 48000 Hz -> 44100 Hz
   a2dp_src: discovery started
   a2dp_src: Target device found → connecting → connected
   app: BT=Streaming | I2S 176400 B/s | buf … | heap …
   ```

3. เปิดวิทยุบน S3 — ทั้ง PCM5102A และลำโพง Bluetooth ควรมีเสียงพร้อมกัน
4. ถอด/ปิดลำโพงแล้วเปิดใหม่ ⇒ auto-reconnect ทำงานเอง
5. อยากเปลี่ยนค่าตั้งค่า ⇒ ปิดลำโพงไว้ รอ 3 นาที ต่อ Wi-Fi `ESP32-A2DP-Setup`
   แล้วเปิด `http://192.168.4.1/`

## ทดสอบแยกส่วน

| ต้องการทดสอบ | วิธี |
|---|---|
| เส้นทางรับ I2S + resampler (ไม่ต้องมี S3/Bluetooth) | เปิด **Loopback self test** → ดู `LOOPBACK PASS: 128 frames …` หรือ `LOOPBACK PASS (resampled 48000 -> 44100 Hz)` |
| Bluetooth + ลำโพง (ไม่ต้องมี S3) | เปิด **Test tone** → `tone: feeding 1000 Hz sine to the A2DP callback` |
| สายที่ต่ออยู่มีสัญญาณจริงไหม | ปล่อย **Check pin activity at boot** ไว้ → log บอก `toggling` / `static LOW` / `static HIGH` ของ BCLK, WS, DATA |
| ลำโพงรุ่นใหม่จับคู่ไม่ติด | เปิด **Use Secure Simple Pairing (SSP)** แล้ว build/flash ใหม่ |

## แก้ปัญหาที่พบบ่อย

| อาการ | สาเหตุ / วิธีแก้ |
|---|---|
| เข้า `192.168.4.1` ไม่ได้ | AP เปิดเฉพาะหลังหาลำโพงไม่เจอ **180 วินาที** — ปิดลำโพง/ให้หลุดแล้วรอ; ดู log `no speaker after 180 s` |
| Windows ต่อ AP ไม่ติด / ไปต่อ Wi-Fi อื่น | Windows อาจจำ profile เดิมที่ cipher ผิด (`netsh wlan delete profile name="ESP32-A2DP-Setup"` แล้วต่อใหม่) และมักเลือก network ที่แรงกว่า — ปิด AP อื่นชั่วคราว |
| `not enough free heap for the config portal` | heap ต่ำกว่า 32 KB แม้ปล่อย Bluetooth แล้ว — บอร์ดลองใหม่ทุก 10 วินาที |
| `BT=Disconnected` วน discovery ทุก ~12.8 s | ปกติเมื่อไม่มีลำโพงเปิดอยู่/ชื่อไม่ตรง — ตรวจชื่อ (เทียบแบบ prefix) |
| เงียบแต่ `BT=Streaming` | `PCM shift bits` ผิด หรือ S3 ส่ง 48k แต่ตั้ง input เป็น 44.1k, หรือ DATA/WS สลับ |
| เสียงแตก/กระตุก | buffer overflow (ดู `drop`) — ลดงานฝั่ง S3, เพิ่ม `Audio buffer`, ตรวจ GND ร่วม |
| `malloc failed size=4112` + `btc_media_aa_prep_sbc_2_send no buffer TxCnt 15` | peer ไม่ drain TX queue ⇒ heap หมดแล้วหลุด — ลด `Audio buffer`, ลำโพงอยู่ใกล้ขึ้น (ปัญหาที่รู้อยู่ ดู STATUS.md) |
| `only … bytes could be allocated contiguously: buffer reduced to …` | ค่า buffer ใหญ่เกิน contiguous ใน internal RAM — บอร์ดลดขนาดให้เองและยังบูตได้ (ไม่ fatal) |
| แฟลชไม่ผ่าน (`Invalid head of packet` / `stopped responding`) | ถอดสาย I2S ของ S3 ออกก่อนแฟลช; ถ้าบอร์ดไม่มีปุ่ม IO0 ให้ใช้ adapter ที่ดึง IO0 ลง GND (หรือ auto-reset ที่ใช้ได้) |

## ข้อจำกัดและสิ่งที่ยังไม่ได้ยืนยัน

- **ยังไม่ได้ยืนยันเสียงด้วยหู** ผ่านห่วงโซ่ S3 → CAM → ลำโพง (ตอนทดสอบได้ `BT=Streaming`
  และ tone ถูกบริโภคจริง แต่ link หลุดหลัง ~10–15 s เพราะ TX buffer ของ stack กิน heap
  จนหมด — อยู่ในหัวข้อ “ปัญหาที่ยังเปิดอยู่” ของ `esp32cam_i2s_a2dp/STATUS.md`)
- ยังไม่ได้ทดสอบต่อเนื่อง 30 นาที (clock drift ระหว่าง S3 กับ BT stack)
- A2DP ออกได้ 44100 Hz เท่านั้น (ข้อจำกัดของ SBC encoder ใน Bluedroid)
- ถ้าแฟลช `firmware.bin` ที่ `0x0` ค่าตั้งค่าและคู่ลำโพงที่จำไว้จะหาย
- ฝั่ง S3 ควรตั้ง `audio.setOutputSampleRate(Audio::SR_44100)` ใน `setup()` (นอก repo นี้)

## เอกสารเพิ่มเติม

- [`esp32cam_i2s_a2dp/README.md`](esp32cam_i2s_a2dp/README.md) — รายละเอียดเชิงลึก:
  ผังสาย, dependency, ทุกตัวเลือกใน menuconfig, ขั้นตอน self-test, ผลทดสอบจริง,
  หมายเหตุเรื่อง PSRAM/heap
- [`esp32cam_i2s_a2dp/STATUS.md`](esp32cam_i2s_a2dp/STATUS.md) — สถานะงาน/handoff:
  อะไรยืนยันบนบอร์ดแล้ว, บั๊กที่เจอและแก้, ปัญหาที่ยังเปิดอยู่, ขั้นตอนแฟลช
- [`knowledge.md`](knowledge.md) — โน้ตไลบรารี ESP32-A2DP และแผนที่โค้ด
- [`promp.txt`](promp.txt) — สเปกโปรเจกต์ต้นฉบับ

## License

โค้ดบริดจ์ (`esp32cam_i2s_a2dp/`) เป็นของโปรเจกต์นี้ ส่วน `ESP32-A2DP-main/`
เป็นไลบรารีของ pschatzmann ภายใต้ **Apache-2.0** (ดู `LICENSE` ในโฟลเดอร์นั้น)
