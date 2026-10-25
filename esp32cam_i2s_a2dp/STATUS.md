# สถานะงาน (handoff) — กลับมาทำต่อจากไฟล์นี้

อัปเดตล่าสุด: 25 ต.ค. 2026 · บอร์ดทดสอบ: ESP32-CAM (ESP32-D0WD) ที่ **COM6**
(รอบล่าสุด: ทดสอบกับลำโพงจริง "BT Speaker" — จับคู่และ stream ได้ พร้อมแก้บั๊ก
2 ตัวที่ทำให้ไม่มีเสียง/แครช ดูหัวข้อ "บั๊กที่แก้ไปแล้ว")

อ่าน `README.md` สำหรับผังสาย/ตัวเลือก/menuconfig ทั้งหมด — ไฟล์นี้สรุปแค่ว่า
**ทำอะไรเสร็จแล้ว, อะไรยังไม่ได้ทำ, และเริ่มต่อยังไง**

## ภาพรวมสถานะ

| ส่วน | สถานะ |
|---|---|
| โค้ดบริดจ์ I2S → A2DP (`main/`) | ✅ เขียนครบ, build ผ่าน |
| Flash ลงบอร์ด COM6 | ✅ ผ่าน (image 865,488 B) |
| บูต + Bluetooth Classic + A2DP/AVRCP | ✅ ผ่าน |
| Self-test เส้นทางรับข้อมูล (loopback, 44.1k) | ✅ PASS (128 frames, L/R ถูก) |
| Self-test เส้นทาง resample (loopback, 48k→44.1k) | ✅ PASS (117 frame ramp, ~3.27/frame) |
| Self-test resampler 48k→44.1k (ระดับฟังก์ชัน) | ✅ PASS (1880/1881.6 frames) |
| จับคู่ + stream ไปลำโพง "BT Speaker" จริง | ⚠️ จับคู่ได้, `BT=Streaming`, tone ป้อนข้อมูลแล้ว — แต่เสียงยังไม่ได้ยืนยันด้วยหู และ link หลุดหลัง ~15 s (heap หมด) |
| รับ PCM จาก ESP32-S3 จริง | ❌ ยังไม่ได้ทดสอบ (ยังไม่ต่อสาย) |
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
# CONFIG_APP_BT_SSP is not set             # เปิดถ้าลำโพงรุ่นใหม่ต้องการ SSP
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
