# Live Human Face Detection + Recognition (CrowPanel Advanced 7" ESP32-P4)

A complete, on-device demo that runs **face detection and face recognition on a live
camera feed** and shows the annotated video on the touchscreen of the
[Elecrow CrowPanel Advanced 7" ESP32-P4 HMI AI Display](https://www.elecrow.com/crowpanel-advanced-7inch-esp32-p4-hmi-ai-display-1024x600-ips-touch-screen-with-wifi-6-compatible-with-arduino-lvgl-micropython.html)
(1024×600).

It fuses two things:

* **Espressif esp-dl** models — `HumanFaceDetect` (MSRMNP) + `HumanFaceRecognizer` (MobileFaceNet).
* **Elecrow's official board BSP** for this exact panel — SC2336 MIPI-CSI camera,
  EK79007 MIPI-DSI LCD, GT911 capacitive touch (pin/timing values come straight from
  Elecrow's `Lesson13-Camera_Real-Time` and `Lesson09` examples).

## What it does

* Streams the SC2336 camera (1024×600 RGB565) to the LCD via an LVGL canvas.
* On a second CPU core, runs face detection every frame and draws a box + 5 facial
  keypoints on each detected face.
* Recognizes each sufficiently-large face against an on-flash database:
  * **green box** = recognized (shows the matched ID + similarity in the status bar)
  * **red box** = unknown
* **ENROLL** button: registers the most prominent (largest, centered) face into the
  database and assigns it the next ID.
* **CLEAR DB** button: erases all enrolled faces.
* The database persists in the on-flash `storage` FAT partition (`/spiflash/face.db`),
  so enrolled faces survive reboots.

## Hardware

| Part        | Detail                                                            |
|-------------|-------------------------------------------------------------------|
| SoC         | ESP32-P4 (+ ESP32-C6 for Wi-Fi, unused here)                      |
| Camera      | SC2336, MIPI-CSI, SCCB on **I2C1** (SCL=GPIO13, SDA=GPIO12)        |
| LCD         | EK79007, MIPI-DSI 2-lane @900 Mbps, 1024×600 RGB565, backlight GPIO31 |
| Touch       | GT911, **I2C0** (SCL=GPIO46, SDA=GPIO45), RST=GPIO40, INT=GPIO42   |
| Power       | LDO3 = 2.5 V (MIPI DPHY), LDO4 = 3.3 V (camera rail)              |
| PSRAM/Flash | 32 MB / 16 MB                                                     |

> The optional 2 MP MIPI-CSI camera module **must be connected** for this demo.

## Build & flash

Requires **ESP-IDF v5.4.x** (this was developed against the v5.4.4 toolchain at
`…/Petpooja_ESP32P4/v5.4.4/esp-idf`).

```bash
# from this directory
idf.py set-target esp32p4
idf.py build
idf.py -p <PORT> flash monitor
```

On first build the IDF Component Manager downloads `esp_video`, `esp_cam_sensor`,
`esp_lcd_ek79007`, `esp_lcd_touch_gt911`, `esp_lvgl_port`, and `lvgl` (8.3.x). The
esp-dl models are pulled from `../../../models/` via `override_path` and are embedded
in the app (flash rodata), so no model partition is needed.

## Usage

1. Flash and let the device boot — the camera feed appears full-screen.
2. Point the camera at a face: a **red** box tracks it.
3. Tap **ENROLL** to register that face. The status line shows `Enrolled. DB now has N face(s)`.
4. Now the same person shows a **green** box with their ID and a similarity score.
5. Tap **CLEAR DB** to wipe all enrolled faces.

## Architecture

```
                 ┌──────────────────────── core 0 ────────────────────────┐
  SC2336 ─CSI──► esp_video/ISP ─► capture task ──► frame_cb:
                                                     • copy clean frame ─┐
                                                     • draw last results │ (to AI)
                                                     • LVGL canvas + refr │
                 └────────────────────────────────────────────────────┼─┘
                                                                        ▼
                 ┌──────────────────────── core 1 ───────────────── ai_task ─┐
                   HumanFaceDetect.run() → boxes/keypoints
                   for each face: HumanFaceRecognizer.recognize()
                   on ENROLL: HumanFaceRecognizer.enroll()  → /spiflash/face.db
                   publish results (mutex) + update status labels
                 └────────────────────────────────────────────────────────────┘
```

Detection runs on the full 1024×600 frame, so overlay coordinates map 1:1 to the
display. The display refreshes at the full camera rate; AI results lag by one AI cycle
(typically tens of ms) but track smoothly.

## Tuning / troubleshooting

* **Byte-order (RGB565) — two independent symptoms, two switches.** The default
  assumption is little-endian for both display and inference (correct for SWAP_SHORT +
  LVGL v8). Diagnose by what you see on first boot:
  * **The live video itself is mis-colored** (e.g. skin looks blue/teal): the camera
    byte order is opposite. Flip *both* — set `FACE_RGB565_BYTE_SWAP` to `1` (fixes the
    overlay color) *and* change `DL_IMAGE_PIX_TYPE_RGB565LE` → `..._RGB565BE` in the two
    `img_t` initializers (fixes inference), in `main/face_processor.cpp`.
  * **Video looks fine and boxes track, but recognition is weak/never matches**: only
    the inference colorspace is off. Change just the `img_t` `pix_type` to `..._RGB565BE`.
  * **Box outline is the wrong color but video is fine**: only flip `FACE_RGB565_BYTE_SWAP`.
* **Recognition too strict/loose**: adjust the recognizer threshold via menuconfig
  (`models: human_face_recognition`) or `MIN_FACE_WIDTH` in `face_processor.cpp`.
* **Lower latency / higher FPS**: switch the detect model to `ESPDET_PICO_224_224_FACE`
  in menuconfig (`models: human_face_detect` → default model).
* **Names instead of numeric IDs**: the recognizer DB keys on `uint16_t` IDs. To show
  names, keep a parallel ID→name map (e.g. in NVS) and look it up in `ui_set_status`.

## Credits

* esp-dl models & inference: Espressif (`models/human_face_detect`, `models/human_face_recognition`).
* Board bring-up (camera / LCD / touch pinout & init): Elecrow CrowPanel Advanced
  7" ESP32-P4 example repository (Lesson13 / Lesson09).
