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
    ├── i2s_receiver.c/.h       # I2S slave RX task + แปลง PCM16
    ├── audio_buffer.c/.h       # SPSC ring buffer (lock-free, atomic)
    ├── resampler.c/.h          # 48k → 44.1k (linear interpolation)
    ├── a2dp_source.cpp/.h      # BluetoothA2DPSource + data callback
    ├── tone_source.c/.h        # โทน 1 kHz สำหรับทดสอบโดยไม่ต้องมี S3
    └── loopback_test.c/.h      # self-test (I2S loopback + resampler)
```

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

ผลการ build จริง:

```
esp32cam_i2s_a2dp.bin binary size 0xd34d0 bytes.
Smallest app partition is 0x300000 bytes. 0x22cb30 bytes (72%) free.
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
| ส่งเสียงไปลำโพงจริง (end-to-end) | ❌ ยังทดสอบไม่ได้ — ไม่มีลำโพง Bluetooth ให้จับคู่ |
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
| ไม่ยอมจับคู่ | ตั้งชื่อลำโพงให้ตรง (เทียบแบบ prefix) และเปิดลำโพงให้อยู่ในโหมด discoverable; ถ้าลำโพงรุ่นใหม่ต้องการ SSP ให้เปิด *Use Secure Simple Pairing (SSP)* ใน menuconfig (ดูหัวข้อ 2.1) — ค่าเริ่มต้นยังใช้ PIN `1234` แบบ legacy |
| จำ peer เก่า | แก้แล้ว: ล้าง NVS ตอนบูตเมื่อระบุชื่อลำโพง |
| flash ไม่ได้ | ESP32-CAM ต้องดึง GPIO0 ลง GND ตอนอัปโหลด (บอร์ดรุ่นมี USB-serial ทำเอง) |

## หมายเหตุเรื่อง PSRAM / หน่วยความจำ

- บอร์ดนี้มี PSRAM แต่โปรเจกต์ **ไม่เปิดใช้** (`CONFIG_SPIRAM` ปิด) เพราะ buffer
  เสียง 64 KB อยู่ใน internal RAM ซึ่งเร็วกว่าและปลอดภัยกับ DMA
  ถ้าต้องการเปิด: `idf.py menuconfig` → Component config → ESP PSRAM → Enable
- heap ว่างหลัง BT ทำงาน ~71 KB — เพียงพอสำหรับ buffer 371 ms

## ข้อจำกัดที่ยังเหลือ

- ยังไม่ได้ยืนยันเสียงจริงจาก S3 → CAM → ลำโพง เพราะไม่มีลำโพง Bluetooth และ
  ไม่ได้ต่อ S3 ในรอบทดสอบนี้
- ยังไม่ได้ทดสอบต่อเนื่อง 30 นาที (clock drift ระหว่าง S3 กับ BT stack จะถูกกลืน
  ด้วย buffer 371 ms แต่ยังไม่ได้วัดจริง)
- เส้นทาง resample 48k→44.1k ตรวจแล้วบนบอร์ด (self-test ผ่าน) แต่ยังไม่ได้
  ทดสอบกับสถานี 48 kHz จริงที่ส่งมาจาก ESP32-S3
- เสียงจริงจาก S3 → CAM → ลำโพง และ soak test 30 นาที ยังค้างอยู่ เพราะไม่มี
  ลำโพง Bluetooth และยังไม่ได้ต่อ S3 เข้ากับบอร์ด
