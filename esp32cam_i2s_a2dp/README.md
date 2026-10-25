# ESP32-CAM: I2S Receiver → Bluetooth A2DP Source

บริดจ์เสียงจาก Internet Radio บน ESP32-S3 ไปยังลำโพง Bluetooth โดยใช้
ESP32-CAM (ESP32 รุ่นแรก) เป็นตัวรับ I2S และส่งต่อด้วย A2DP

```
Internet Radio / MP3 บน ESP32-S3
      │  I2S (S3 เป็น Master: BCLK/WS/DATA)
      ▼
ESP32-CAM  I2S Slave RX  →  แปลง 32-bit slot เป็น PCM16 stereo
      │                       → audio buffer (SPSC ring buffer)
      ▼
Bluetooth A2DP Source  →  ลำโพง Bluetooth
```

> PCM5102A ยังทำงานเหมือนเดิม เพราะ ESP32-S3 ยังขับ I2S เส้นเดิม และเราแค่
> **branch** สัญญาณ 3 เส้นไปที่ ESP32-CAM แบบขนาน (CAM ไม่สร้าง clock)

## ฮาร์ดแวร์ที่ใช้ทดสอบจริง

| รายการ | ค่าที่อ่านได้จากบอร์ด |
|---|---|
| Chip | ESP32-D0WD rev v1.0 (Bluetooth Classic ✓) |
| Flash | 4 MB (DIO, 40 MHz) |
| Cores / clock | 2 cores / 160 MHz |
| BT MAC | `30:ae:a4:d8:41:86` |
| พอร์ตที่ใช้ทดสอบ | `COM6` |
| Framework | ESP-IDF v5.5.6, xtensa-esp-elf 14.2.0 |

## ผังการต่อสาย

| ESP32-S3 (PCM5102A เดิม) | ESP32-CAM | หน้าที่ |
|---|---|---|
| GPIO1 (BCLK) | GPIO14 | I2S BCLK (S3 → CAM) |
| GPIO2 (LRCK/WS) | GPIO13 | I2S WS (S3 → CAM) |
| GPIO40 (DATA) | GPIO4 | I2S DATA (S3 → CAM) |
| GND | GND | Ground ร่วม (จำเป็น) |

ข้อควรระวัง (ตรวจกับบอร์ด AI-Thinker แล้ว):

- **ห้ามต่อ CAM เป็น master** — S3 ต้องเป็นผู้สร้าง BCLK/WS เพียงเจ้าเดียว
  ถ้า CAM สร้าง clock จะชนกันและเสียงจะเพี้ยน/เงียบ
- GPIO4 บน ESP32-CAM ต่อกับวงจร Flash LED → LED อาจสว่างวูบวาบและกวนเส้น DATA
  เล็กน้อย ถ้าเสียงแตกให้ย้าย DATA ไป GPIO13/15/2 (ขา SD ที่ไม่ได้ใช้) แล้วแก้ใน
  `idf.py menuconfig`
- GPIO13/14/4 เป็นขาของช่อง microSD ด้วย — โปรเจกต์นี้ไม่ใช้ SD จึงใช้ได้
- ห้ามใช้ GPIO16/17 (PSRAM) และเลี่ยง GPIO0, 2, 12, 15 (strapping)
- ห้ามป้อน 5V เข้า GPIO ใด ๆ
- ไม่ต้องถอดหรือแก้สาย PCM5102A

## โครงสร้างโปรเจกต์

```
esp32cam_i2s_a2dp/
├── CMakeLists.txt              # EXTRA_COMPONENT_DIRS = components/
├── partitions.csv              # app 3 MB (image จริง ~865 KB)
├── sdkconfig.defaults          # target esp32, BT Classic+A2DP, ไม่เปิด BLE
├── components/                 # (ไม่ commit) dependency ภายนอก
│   ├── ESP32-A2DP/             # ไลบรารีนี้ (คัดลอกจาก ../ESP32-A2DP-main)
│   └── arduino-audio-tools/    # ไลบรารีที่ ESP32-A2DP บังคับใน CMakeLists
└── main/
    ├── main.cpp                # app_main: boot info, สถานะทุก 5 วินาที
    ├── app_config.h            # แปลง Kconfig → ค่าคงที่
    ├── Kconfig.projbuild       # ตัวเลือกใน menuconfig
    ├── app_settings.c/.h       # ค่าตั้งค่าใน NVS (แก้จากหน้าเว็บได้)
    ├── i2s_receiver.c/.h       # I2S slave RX task + แปลง PCM16
    ├── audio_buffer.c/.h       # SPSC ring buffer (lock-free, atomic)
    ├── resampler.c/.h          # 48k → 44.1k (linear interpolation)
    ├── a2dp_source.cpp/.h      # BluetoothA2DPSource + data callback
    ├── bt_scan.c/.h            # แคชรายชื่อลำโพงที่ discovery เจอ
    ├── config_portal.c/.h      # Wi-Fi AP + HTTP server โหมดตั้งค่า
    ├── web/index.html          # หน้าเว็บ (ฝังใน firmware ด้วย EMBED_FILES)
    ├── pin_activity.c/.h       # ตรวจว่า BCLK/WS/DATA มีสัญญาณตอนบูต
    ├── tone_source.c/.h        # โทน 1 kHz สำหรับทดสอบโดยไม่ต้องมี S3
    └── loopback_test.c/.h      # self-test (I2S loopback + resampler)
```

## ตั้งค่าผ่าน Wi-Fi (AP mode)

ค่าที่เคยต้องแก้ใน `menuconfig` แล้ว build ใหม่ ตอนนี้แก้จากหน้าเว็บได้
(เก็บใน NVS namespace `appcfg`):

| ตั้งค่าได้ | หมายเหตุ |
|---|---|
| ชื่อลำโพงเป้าหมาย | เลือกจากรายการที่สแกนเจอ หรือพิมพ์เอง (เทียบแบบขึ้นต้นด้วย) |
| ชื่อ Bluetooth ของบอร์ด | ชื่อที่ประกาศออกไป |
| SSP เปิด/ปิด | สำหรับลำโพงรุ่นเก่าที่ใช้ PIN แบบเดิม |
| I2S input rate | 44100 / 48000 Hz (ต้องตรงกับที่ ESP32-S3 ส่งมา) |
| ขนาด jitter buffer | 50–2000 ms |

**AP เปิดเมื่อไหร่** — บอร์ดบูตเข้าโหมด Bluetooth อย่างเดียว ไม่เปิด Wi-Fi ตั้งแต่แรก
ถ้าต่อลำโพงไม่ได้ภายใน **3 นาที** (`CONFIG_APP_PORTAL_DELAY_S`) จะ

1. ปิด Bluetooth ทั้งก้อนด้วย `end(true)` — disable/deinit Bluedroid + controller
   และคืนหน่วยความจำ Classic BT ให้ระบบ
2. คืน ring buffer ~64 KB
3. จึงเปิด AP + HTTP server

เปิด Bluetooth กลับไม่ได้ในรอบบูตนั้น (หน่วยความจำถูกปล่อยไปแล้ว) — AP จะค้างอยู่จนกว่า
จะกด “บันทึกและรีสตาร์ท” หรือรีสตาร์ทบอร์ดเอง

```
SSID     : ESP32-A2DP-Setup
Password : esp32cam
URL      : http://192.168.4.1/
```

- **จะเข้าไปตั้งค่าได้ต้องให้บอร์ดหาลำโพงไม่เจอ 3 นาที** (ปิดลำโพงไว้ หรือให้หลุดการเชื่อมต่อ)
  ระหว่างนั้นบอร์ดยังพยายามค้นหา/ต่อลำโพงตามปกติ
- รายการลำโพงในหน้าเว็บมาจากการค้นหาช่วง 3 นาทีก่อนหน้า (ตอน AP ขึ้น Bluetooth ปิดแล้ว
  จึงกด “ค้นหาใหม่” ไม่ได้ — พิมพ์ชื่อลำโพงเองได้)
- กด “บันทึกและรีสตาร์ท” แล้วบอร์ดรีบูต 1 รอบ ค่าใหม่มีผลทันทีหลังบูต
- หน้าสถานะ (`/api/state`) และรายการอุปกรณ์ที่สแกนเจอ รีเฟรชเองทุก 2 วินาที
- **A2DP ส่งออก 44100 Hz คงที่** — SBC encoder ของ Bluedroid ฮาร์ดโค้ด PCM เป็น 44100
  (`btc_a2dp_source.c`: `media_feeding.cfg.pcm.sampling_freq = 44100`) หน้าเว็บจึงแสดง
  ค่านี้แบบอ่านอย่างเดียว ถ้าเปลี่ยนเป็น 48000 เสียงจะเร็วขึ้น ~9%
- ตอน AP เปิดอยู่ Bluetooth ถูกปิดไปแล้ว จึงไม่มี coex ให้กวนกัน (เป็นการเปลี่ยนจากบิลด์ก่อน
  ที่เปิด AP ทับ Bluetooth)

## ติดตั้ง dependency

```bash
cd esp32cam_i2s_a2dp
mkdir -p components

# ไลบรารีนี้ (จะคัดลอก working copy ปัจจุบัน หรือ clone จาก GitHub ก็ได้)
cp -r ../ESP32-A2DP-main components/ESP32-A2DP

# AudioTools — บังคับโดย CMakeLists.txt ของ ESP32-A2DP (REQUIRES arduino-audio-tools)
git clone --depth 1 https://github.com/pschatzmann/arduino-audio-tools.git \
    components/arduino-audio-tools
```

> `components/` ถูก `.gitignore` ไว้ ถ้า clone โปรเจกต์ใหม่ต้องทำขั้นนี้ก่อน build

## Build / Flash / Monitor

```bash
idf.py set-target esp32          # ทำครั้งแรก (sdkconfig.defaults ระบุ esp32 ไว้แล้ว)
idf.py build
idf.py -p COM6 flash monitor     # หรือแยกเป็น flash แล้ว monitor
```

ถ้าใช้ ESP-IDF ปกติบน Windows ให้เปิด shell จาก **ESP-IDF 5.5 CMD/PowerShell**
ในเครื่องนี้ติดตั้ง IDF ไว้ที่ `C:\esp\esp-idf` (tools: `C:\esp\espressif`,
Python 3.12 จาก uv) ถ้าเรียกจาก Git Bash ให้ใช้ `C:\esp\run_idf.py`
เพราะ `idf.py` จะไม่ทำงานเมื่อมีตัวแปร `MSYSTEM`

### ไฟล์ image เดียว (firmware.bin)

`firmware.bin` ในโฟลเดอร์โปรเจกต์คือ image ที่รวม **bootloader + partition table +
application** ไว้ไฟล์เดียว สำหรับแจกจ่ายหรือแฟลชบอร์ดเปล่า:

```bash
# สร้างใหม่ (ทำอัตโนมัติหลัง build ใด ๆ ก็ใช้คำสั่งนี้)
cd build
esptool.py --chip esp32 merge_bin -o ../firmware.bin \
  --flash_mode dio --flash_freq 40m --flash_size 4MB \
  0x1000 bootloader/bootloader.bin \
  0x8000 partition_table/partition-table.bin \
  0x10000 esp32cam_i2s_a2dp.bin

# แฟลชทั้งไฟล์ที่ offset 0x0
cd ..
esptool.py --chip esp32 -p COM6 -b 460800 write_flash 0x0 firmware.bin
```

ข้อควรทราบ: ไฟล์นี้เติมช่วงว่างระหว่าง partition table กับ app ด้วย `0xFF` ⇒
แฟลชทับแล้ว **NVS (ค่าตั้งค่าและคู่ Bluetooth) จะว่างทั้งหมด** บอร์ดจะกลับไปใช้ค่า
จาก Kconfig แล้วต้องจับคู่ลำโพงใหม่ (หรือตั้งผ่านหน้าเว็บ)

ผลการ build จริง (รอบล่าสุด รวม Wi-Fi config portal):

```
esp32cam_i2s_a2dp.bin binary size 0x15e640 bytes (1.4 MB)
Smallest app partition is 0x300000 bytes. 0x1a19c0 bytes (54%) free.
firmware.bin           1500736 bytes, flash at 0x0
```

## ตัวเลือกใน menuconfig

`idf.py menuconfig` → **ESP32-CAM I2S to A2DP bridge**

| ตัวเลือก | ค่าเริ่มต้น | ความหมาย |
|---|---|---|
| Local Bluetooth name | `ESP32-CAM-A2DP` | ชื่อที่ CAM ประกาศ |
| Bluetooth speaker name | `BT-Speaker` | ชื่อลำโพงที่ต้องไปค้นหา (เทียบแบบ prefix) — **ต้องแก้เป็นชื่อจริง**; เว้นว่าง = ต่อกับ sink ตัวแรกที่เจอ |
| A2DP sample rate | 44100 | PCM ที่ส่งให้ลำโพง |
| I2S input sample rate | 44100 | ต้องตรงกับที่ S3 ส่ง; ถ้าไม่ตรงจะ resample อัตโนมัติ |
| I2S BCLK/WS/DATA GPIO | 14/13/4 | ตามผังด้านบน |
| PCM shift bits | 16 | 16 = PCM16 อยู่ในครึ่งบนของ slot 32-bit (ตามที่ ESP32-audioI2S ทำ) |
| Secure Simple Pairing (SSP) | ปิด | เปิดเมื่อลำโพงรุ่นใหม่ไม่ยอมจับคู่ด้วย PIN แบบ legacy (ดูหัวข้อ 2.1) |
| Audio buffer | 300 ms | จริงได้ 16384 frames = **371 ms** (ปัดเป็น power of 2) |
| Loopback self test | ปิด | เปิดเพื่อทดสอบเส้นทาง I2S ตอนบูต |
| Test tone | ปิด | สร้างโทน 1 kHz แทน I2S (ทดสอบ Bluetooth โดยไม่ต้องมี S3) |

## ทดสอบแยกส่วน

### 1) Self-test ของเส้นทางรับข้อมูล (ไม่ต้องมี S3 และไม่ต้องมี Bluetooth)

เปิด `Loopback self test` แล้ว build/flash ⇒ ตอนบูตบอร์ดจะใช้ I2S1 เป็น master TX
ขับ BCLK/WS/DATA บนขาเดียวกับตัวรับ (loopback ภายใน GPIO matrix) แล้วตรวจข้อมูลที่
เข้า audio buffer จริง:

```
I (537) i2s_rx: slave RX: BCLK=14 WS=13 DIN=4, 32-bit slots, shift=16
I (561) loopback: RESAMPLER PASS: 1880/1881.6 frames, step 10..11
I (817) loopback: LOOPBACK PASS: 128 frames verified (32-bit slots, shift=16, L/R order correct)
```

- `RESAMPLER PASS` = ฟังก์ชันแปลง 48k→44.1k คืนจำนวน frame ถูกต้อง (1880 จาก
  1881.6 ที่คำนวณได้) และ ramp ยังเป็นเส้นตรง
- `LOOPBACK PASS` = ข้อมูล 128 frame ที่รับเข้ามาแล้วแปลงเป็น PCM16 ตรงกับที่ส่ง
  ออกไปทุก sample รวมทั้ง **ลำดับ L/R ไม่สลับ**
- ข้อจำกัด: loopback ใช้การจัดวางบิตแบบเดียวกับที่ตั้งไว้ใน `PCM shift bits`
  จึงยืนยัน "ไปป์ไลน์ + ลำดับช่อง" ได้ แต่ไม่ได้ยืนยันว่าฝั่ง S3 วางบิตแบบนั้นจริง
  (ดูหัวข้อ "ตรวจ 32-bit slot" ด้านล่าง)

#### 1.1) Self-test เส้นทาง resample (input 48000 → A2DP 44100)

ตั้ง *I2S input sample rate* = 48000 (เปิด *Loopback self test* ด้วย) ⇒ ข้อมูลจริง
จะผ่าน `resampler_process()` ในเส้นทางเสียง แล้วเข้าสู่ audio buffer. self-test จะ
ตรวจ "ความหนาแน่น" ของ ramp แทนการเทียบค่าตรง ๆ เพราะ interpolation อาจเริ่มที่
ตำแหน่งเศษส่วนใดก็ได้ (ขึ้นกับว่า I2S read แรกไปตกที่ frame ไหน)

```
I (826) loopback: LOOPBACK PASS (resampled 48000 -> 44100 Hz): 117 frame ramp, ~3.27 per output frame (shift=16, L/R order correct)
```

- `117 frame ramp` = 128 input frame ที่อัตราส่วน 44.1/48 ควรเหลือ ≈117 frame
  (เกณฑ์ที่ยอมรับ ±6 frame)
- `~3.27 per output frame` = ramp เพิ่ม 3 ต่อ input frame × step 1.088
- ถ้า resampler ไม่ถูกเรียก หรืออัตราส่วนผิด จำนวน frame ของ ramp จะหลุดเกณฑ์
  ทันที (เช่นไม่ resample เลยจะได้ 128 frame)
- ทุก frame ยังถูกตรวจว่า R = -L (ลำดับ L/R ไม่สลับ) รวมช่วงหัว/ท้ายของ ramp

### 2) ทดสอบ Bluetooth ด้วยโทน 1 kHz (ไม่ต้องมี S3)

เปิด `Test tone`, ตั้งชื่อลำโพงให้ถูก, flash แล้วเปิดลำโพง ⇒ ควรได้ยินเสียงโทน
และ log จะขึ้น `tone: feeding 1000 Hz sine to the A2DP callback`

#### 2.1) ถ้าลำโพงไม่ยอมจับคู่ — เปิด Secure Simple Pairing (SSP)

ลำโพงรุ่นใหม่ส่วนใหญ่เลิกใช้ Legacy Pairing (PIN `1234`) แล้ว และจะจับคู่ผ่าน
**Secure Simple Pairing** เท่านั้น อาการคือเห็น `BT=Connecting` / หลุดบ่อย
หรือ discovery วนซ้ำโดยไม่เคยขึ้น `Connected`

เปิด `idf.py menuconfig` → *ESP32-CAM I2S to A2DP bridge* →
**Use Secure Simple Pairing (SSP)** ⇒ build/flash ใหม่ แล้ว log ตอนบูตจะเป็น

```
I (535) a2dp_src: pairing: SSP enabled (numeric value auto-confirmed)
```

สิ่งที่เกิดขึ้น:

- ไลบรารีตั้ง IO capability เป็น `ESP_BT_IO_CAP_IO` (มีจอ + ปุ่ม yes/no) และ
  ตั้ง `ssp_en` ใน bluedroid config ไว้แล้ว
- ESP32-CAM ไม่มีจอและไม่มีคีย์แพด ⇒ เมื่อมี `ESP_BT_GAP_CFM_REQ_EVT`
  (numeric comparison) บอร์ดจะตอบ "ยืนยัน" ให้อัตโนมัติ และ
  `ESP_BT_GAP_KEY_NOTIF_EVT` (passkey ที่ต้องให้อีกฝ่ายกรอก) จะถูก log ไว้เฉย ๆ
- ถ้าลำโพงเก่าจับคู่ด้วย PIN ได้อยู่แล้ว ไม่ต้องเปิดตัวเลือกนี้ (ค่าเริ่มต้น = ปิด)
  เพราะการเปิด SSP จะเปลี่ยนวิธีจับคู่ของทั้งบอร์ด

> ยังไม่ได้ทดสอบกับลำโพงจริงในรอบนี้ (ไม่มีฮาร์ดแวร์) — ยืนยันได้แค่ว่าเปิดแล้ว
> stack ยังขึ้นปกติ (`A2DP Enable with AVRC`), heap เท่าเดิม และค่า IO capability
> ถูกตั้งก่อน `start()` ตามที่ไลบรารีต้องการ

### 3) ทดสอบระบบจริง

1. ต่อสายตามผัง (S3 → CAM) โดยยังคง PCM5102A ไว้
2. เปิด S3 ให้เล่นวิทยุ → PCM5102A ต้องดังเหมือนเดิม
3. CAM ควรขึ้น `BT=Streaming` และ `I2S` มี byte/s ประมาณ `176400 B/s`
   (44100 Hz × 2 ch × 2 byte)
4. เปิด/ปิดสถานี เปลี่ยนสถานี ทดสอบ 44.1k และ 48k
5. ถอดปลั๊กลำโพงแล้วเปิดใหม่ → auto-reconnect ทำงาน

## ผลการทดสอบที่ทำได้จริงในรอบนี้

| การทดสอบ | ผล |
|---|---|
| `idf.py build` (ESP-IDF 5.5.6, target esp32) | ✅ ผ่าน, image 865 KB |
| Flash ทาง COM6 (esptool 4.12) | ✅ ผ่าน, hash verified |
| บูต + Bluetooth Classic + A2DP/AVRCP init | ✅ `BT enabled`, `A2DP Enable with AVRC` |
| I2S slave RX เริ่มทำงาน (core 1) | ✅ `slave RX: BCLK=14 WS=13 DIN=4, 32-bit slots, shift=16` |
| Loopback self-test 44100 → 44100 (128 frames, เทียบค่าตรงทุก sample) | ✅ PASS |
| Loopback self-test 48000 → 44100 (117 frame ramp ผ่านเส้นทาง resample จริง) | ✅ PASS |
| ตัวเลือก SSP เปิด/ปิด (build + บูตบนบอร์ด) | ✅ ผ่านทั้งสองแบบ (`SSP enabled`, `SSP disabled`), heap ~71 KB เท่าเดิม |
| Resampler 48k→44.1k self-test (ระดับฟังก์ชัน) | ✅ PASS |
| ค้นหาลำโพง (discovery) | ⚠️ เริ่ม discovery ซ้ำทุก ~12.8 วินาที แต่ไม่พบอุปกรณ์ (ไม่มีลำโพงเปิดอยู่ใกล้บอร์ด) |
| จับคู่กับลำโพงจริง "BT Speaker" (`00:58:56:74:31:f3`, RSSI -60) | ✅ `Target device found` → `a2dp connecting to peer` → `connected` |
| เริ่มเล่นเสียง (tone 1 kHz) ผ่าน A2DP | ⚠️ `audio state: started` + `BT=Streaming`, tone ป้อน PCM และ BT บริโภคทัน (buffer 2–16%, underflow คงที่) แต่หลัง ~10–15 s link หลุด (heap หมดจาก TX buffer 4112 B × 15) — ยังไม่ได้ยืนยันเสียงด้วยหู |
| รับข้อมูลจาก ESP32-S3 จริง | ❌ ยังทดสอบไม่ได้ — ไม่ได้ต่อ S3 เข้ากับบอร์ด |
| ทดสอบต่อเนื่อง 30 นาที | ❌ ยังไม่ได้ทำ |

ระหว่างทดสอบพบบั๊กจริง 1 จุดและแก้แล้ว: บอร์ดยังจำ peer เก่าใน NVS
(`a7:d3:a7:f2:59:97`) แล้ววน reconnect หา peer นั้นตลอดโดยไม่ยอมค้นหาลำโพงที่ตั้งไว้
⇒ เพิ่มการล้างรายการ peer ที่เก็บไว้ตอนบูตเมื่อมีการระบุชื่อลำโพง
(log: `a2dp_src: cleared the stored peer address`)

## สถานะที่เห็นใน log

```
app: BT=Disconnected | I2S      0 B/s | buf   0% (0/16384 frames) | drop 0 fr, underflow 0 fr | timeouts 99, errors 0 | heap 71016
```

- `timeouts` เพิ่มตลอดเมื่อ S3 ไม่ได้ต่อ → **ปกติ** (ไม่มี BCLK จึงอ่าน I2S ไม่ได้)
- `drop` = chunk ที่ producer ทิ้งเพราะ buffer เต็ม (BT ช้ากว่าที่รับมา)
- `underflow` = จำนวน frame ที่ callback ต้องเติม silence เพราะ buffer ไม่พอ
- `buf %` = ระดับ buffer ปัจจุบัน ควรแกว่งต่ำ ๆ (ไม่ใช่ 100%)

## ตรวจ 32-bit slot / บิต shift

`ESP32-audioI2S` v3.4.7 ตั้งค่า I2S เป็น

```cpp
m_i2s_std_cfg.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
                             I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO);
```

(`src/Audio.cpp` บรรทัด ~5845) คือ slot 32-bit และข้อมูล PCM16 ถูกวางไว้ครึ่งบน
ดังนั้นฝั่ง CAM จึงรับข้อมูลเป็น 32-bit แล้วใช้ `int16_t sample = (int16_t)(raw >> 16);`
(ค่าเริ่มต้น `PCM shift bits = 16`)

ถ้าเสียงที่ได้เบามาก/แทบไม่มีสัญญาณ ให้ลองตั้ง `PCM shift bits = 0`
(กรณีข้อมูลชิดขวา) แล้วฟังเทียบ — self-test จะช่วยบอกได้ว่าฝั่ง CAM ทำงานปกติ

## สิ่งที่ต้องแก้ฝั่ง ESP32-S3 (และเหตุผล)

1. **เพิ่มการบังคับ output 44.1 kHz** — ใน `setup()` ของโปรเจกต์เดิม หลังตั้ง pinout:

   ```cpp
   audio.setOutputSampleRate(Audio::SR_44100);   // v3.4.7: Audio.h บรรทัด ~163
   ```

   `Audio::OutputSR_t` มี `SR_ORIGIN / SR_44100 / SR_48000` และฟังก์ชันนี้เรียก
   `reconfigI2S()` พร้อม resampler ภายใน (Audio.cpp บรรทัด ~6410) ทำให้ทุกสถานี
   ออกที่ 44.1 kHz ตรงกับ A2DP และไม่ต้องพึ่ง resampler ฝั่ง CAM
   **ถ้าไม่แก้**: ตั้ง `I2S input sample rate = 48000` ใน menuconfig ของ CAM
   เพื่อให้ตัว resampler ทำงานแทน (คุณภาพพอใช้ แต่กิน CPU และมี latency เพิ่ม)
2. **ไม่ต้องแก้ขา/โค้ดอื่น** — BCLK/WS/DATA เดิม (GPIO1/2/40) ยังขับ PCM5102A
   ตามปกติ แค่ต่อสายขนานไป CAM เท่านั้น ห้ามลบหรือย้าย PCM5102A
3. ไม่ต้องแก้ `platformio.ini` (ไม่มีการเพิ่ม library ฝั่ง S3)

## ปัญหาที่อาจเจอ

| อาการ | สาเหตุ/วิธีแก้ |
|---|---|
| `timeouts` เพิ่มแต่ `BT=Streaming` | S3 ไม่ได้ส่ง clock หรือต่อสาย BCLK/WS/DATA ผิด/ขาด GND |
| `I2S 0 B/s` ตลอด | เช่นเดียวกับข้างบน; ลองเปิด loopback test เพื่อแยกฝั่ง CAM |
| เสียงแตก/กระตุก | buffer overflow (ดู `drop`) — ลดงานฝั่ง S3 หรือเพิ่ม `Audio buffer`; ตรวจ GND ร่วม |
| เงียบแต่ `BT=Streaming` | `PCM shift bits` ผิด หรือ S3 ส่ง 48k แต่ตั้ง input เป็น 44.1k |
| `BT_OSI: malloc failed ... size=4112` + `btc_media_aa_prep_sbc_2_send no buffer TxCnt 15` | stack จอง TX buffer 4112 B ต่อ packet แล้ว peer ไม่ drain / link อ่อน ⇒ heap หมดแล้วหลุด — ลด `Audio buffer` เพื่อคืน RAM และดูระยะ/RSSI ของลำโพง |
| ไม่ยอมจับคู่ | ตั้งชื่อลำโพงให้ตรง (เทียบแบบ prefix) และเปิดลำโพงให้อยู่ในโหมด discoverable; ถ้าลำโพงรุ่นใหม่ต้องการ SSP ให้เปิด *Use Secure Simple Pairing (SSP)* ใน menuconfig (ดูหัวข้อ 2.1) — ค่าเริ่มต้นยังใช้ PIN `1234` แบบ legacy |
| จำ peer เก่า | แก้แล้ว: ล้าง NVS ตอนบูตเมื่อระบุชื่อลำโพง |
| flash ไม่ได้ | ESP32-CAM ต้องดึง GPIO0 ลง GND ตอนอัปโหลด (บอร์ดรุ่นมี USB-serial ทำเอง); ถ้า esptool ขึ้น `Invalid head of packet (0x65)` / `The chip stopped responding` ให้ถอดสาย I2S ของ S3 ออกก่อนแฟลช — ไฟเลี้ยงจะทรุดตอนบอร์ดรับงานทั้ง I2S + BT |
| เข้า `192.168.4.1` ไม่ได้ | AP เปิดเฉพาะหลังบอร์ดหาลำโพงไม่เจอ **3 นาที** — ปิดลำโพง (หรือให้หลุด) แล้วรอ; ดู log `no speaker after 180 s: switching to the config portal` |
| `not enough free heap for the config portal` | heap ต่ำกว่า 32 KB หลังปล่อย Bluetooth แล้ว บอร์ดจะลองใหม่ทุก 10 วินาที |
| หน้าเว็บค้างหลังกด “บันทึก” | ปกติ: บอร์ดรีสตาร์ทหลังตอบกลับ 1.2 วินาที แล้วกลับไปโหมด Bluetooth (AP หายไป) — ถ้าจะตั้งค่าใหม่ต้องให้หาลำโพงไม่เจอ 3 นาทีอีกครั้ง |

## หมายเหตุเรื่อง PSRAM / หน่วยความจำ

- บอร์ดนี้มี PSRAM แต่โปรเจกต์ **ไม่เปิดใช้** (`CONFIG_SPIRAM` ปิด) เพราะ buffer
  เสียง 64 KB อยู่ใน internal RAM ซึ่งเร็วกว่าและปลอดภัยกับ DMA
  ถ้าต้องการเปิด: `idf.py menuconfig` → Component config → ESP PSRAM → Enable
- heap ว่างหลัง BT ทำงาน ~71 KB — เพียงพอสำหรับ buffer 371 ms
- Wi-Fi เปิดเฉพาะโหมดตั้งค่า **หลังปิด Bluetooth แล้ว** จึงไม่แย่ง heap กับ stack BT
  เหมือนบิลด์ก่อน (ตอนนั้น AP เปิดคู่กับ BT แล้วเหลือ heap ~40 KB ซึ่งไม่พอ)
- ก่อนเปิด AP บอร์ด **คืน ring buffer 64 KB** (โหมดตั้งค่าไม่มีเสียงอยู่แล้วเพราะ gate ปิด)
  และไม่จองคืน เพราะ Bluetooth ที่จะใช้ buffer ปล่อยหน่วยความจำไปแล้ว
  (`end(true)` = `esp_bt_controller_mem_release`)
- อย่าลด `CONFIG_ESP_WIFI_MGMT_SBUF_NUM` / `DYNAMIC_*_BUFFER_NUM` ต่ำกว่าค่าใน
  `sdkconfig.defaults` — ทดสอบแล้วว่า AP จะไม่ส่ง beacon ทำให้มือถือสแกนไม่เห็น
- เพื่อให้แอปพอดี IRAM: ปิด `CONFIG_ESP_WIFI_IRAM_OPT` / `CONFIG_ESP_WIFI_RX_IRAM_OPT`
  และลด Wi-Fi buffer ลง (ดู `sdkconfig.defaults`) — ไม่กระทบคุณภาพเสียงเพราะ Wi-Fi
  ไม่ทำงานตอนสตรีม

## ข้อจำกัดที่ยังเหลือ

- ยังไม่ได้ยืนยันเสียงจริงจาก S3 → CAM → ลำโพง เพราะไม่มีลำโพง Bluetooth และ
  ไม่ได้ต่อ S3 ในรอบทดสอบนี้
- ยังไม่ได้ทดสอบต่อเนื่อง 30 นาที (clock drift ระหว่าง S3 กับ BT stack จะถูกกลืน
  ด้วย buffer 371 ms แต่ยังไม่ได้วัดจริง)
- เส้นทาง resample 48k→44.1k ตรวจแล้วบนบอร์ด (self-test ผ่าน) แต่ยังไม่ได้
  ทดสอบกับสถานี 48 kHz จริงที่ส่งมาจาก ESP32-S3
- เสียงจริงจาก S3 → CAM → ลำโพง และ soak test 30 นาที ยังค้างอยู่ เพราะไม่มี
  ลำโพง Bluetooth และยังไม่ได้ต่อ S3 เข้ากับบอร์ด
