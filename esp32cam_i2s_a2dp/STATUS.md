# สถานะงาน (handoff) — กลับมาทำต่อจากไฟล์นี้

อัปเดตล่าสุด: 25 ต.ค. 2026 · บอร์ดทดสอบ: ESP32-CAM (ESP32-D0WD) ที่ **COM6**

อ่าน `README.md` สำหรับผังสาย/ตัวเลือก/menuconfig ทั้งหมด — ไฟล์นี้สรุปแค่ว่า
**ทำอะไรเสร็จแล้ว, อะไรยังไม่ได้ทำ, และเริ่มต่อยังไง**

## ภาพรวมสถานะ

| ส่วน | สถานะ |
|---|---|
| โค้ดบริดจ์ I2S → A2DP (`main/`) | ✅ เขียนครบ, build ผ่าน |
| Flash ลงบอร์ด COM6 | ✅ ผ่าน (image 865,488 B) |
| บูต + Bluetooth Classic + A2DP/AVRCP | ✅ ผ่าน |
| Self-test เส้นทางรับข้อมูล (loopback) | ✅ PASS (128 frames, L/R ถูก) |
| Self-test resampler 48k→44.1k | ✅ PASS (1880/1881.6 frames) |
| จับคู่ + ส่งเสียงไปลำโพงจริง | ❌ ยังไม่ได้ทดสอบ (ไม่มีลำโพง BT) |
| รับ PCM จาก ESP32-S3 จริง | ❌ ยังไม่ได้ทดสอบ (ยังไม่ต่อสาย) |
| Soak test 30 นาที | ❌ ยังไม่ได้ทำ |
| แก้ฝั่งโปรเจกต์ S3 (`setOutputSampleRate`) | ❌ ยังไม่ได้แก้ (อยู่นอก repo นี้) |

## สิ่งที่ต้องทำต่อ (เรียงตามลำดับ)

1. **ใส่ชื่อลำโพงจริง** — ตอนนี้ `CONFIG_APP_BT_TARGET_NAME` ยังเป็นค่า default
   `"BT-Speaker"` ⇒ `idf.py menuconfig` → *ESP32-CAM I2S to A2DP bridge* →
   *Bluetooth speaker name to connect to* (ต้องเป็น prefix)
2. **ทดสอบ Bluetooth แบบไม่ต้องมี S3** — เปิด *Use a generated 1 kHz test tone*
   แล้ว flash; เปิดลำโพงตอนบูต ควรได้ยินโทน 1 kHz
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
CONFIG_APP_BT_TARGET_NAME="BT-Speaker"   # ← ยังต้องแก้
CONFIG_APP_OUTPUT_RATE_44100=y
CONFIG_APP_INPUT_RATE_44100=y
CONFIG_APP_I2S_BCLK_GPIO=14  WS=13  DATA=4
CONFIG_APP_I2S_PCM_SHIFT_BITS=16
CONFIG_APP_AUDIO_BUFFER_MS=300           # ได้จริง 16384 frames = 371 ms
# CONFIG_APP_LOOPBACK_TEST is not set     # เปิดชั่วคราวเพื่อ self-test
# CONFIG_APP_TEST_TONE is not set         # เปิดเพื่อทดสอบ BT ไม่ต้องมี S3
# CONFIG_SPIRAM is not set                # บอร์ดมี PSRAM แต่ยังไม่เปิด (ไม่จำเป็น)
```

## บั๊กที่แก้ไปแล้วในรอบนี้

บอร์ดจำ peer เก่าใน NVS (`a7:d3:a7:f2:59:97`) แล้ววน reconnect หา peer นั้น
ไม่ยอมค้นหาลำโพงที่ตั้งไว้ ⇒ `main/main.cpp` ล้าง peer ที่เก็บไว้ตอนบูตเมื่อมี
การระบุชื่อลำโพง (log: `a2dp_src: cleared the stored peer address`)

## ข้อสังเกตที่เจอตอนทดสอบ (ยังไม่ต้องแก้)

- `timeouts` ใน log เพิ่มตลอดเมื่อไม่มี S3 ต่ออยู่ — **ปกติ** เพราะไม่มี BCLK
- `app: BT=Disconnected | I2S 0 B/s | buf 0% | heap ~71 KB` คือสภาพ idle ปกติ
- Discovery วนซ้ำทุก ~12.8 วินาทีเมื่อหาลำโพงไม่เจอ — พฤติกรรมของไลบรารี
