# Live Face Detection + Recognition — R&D bench & attendance kiosk (CrowPanel 7" ESP32‑P4)

On‑device **face detection + recognition on a live camera feed**, shown on the touchscreen of the
[Elecrow CrowPanel Advanced 7" ESP32‑P4 HMI AI Display](https://www.elecrow.com/) (1024×600). Built on
**Espressif esp‑dl** models fused with Elecrow's board BSP (SC2336 camera, EK79007 LCD, GT911 touch).

It runs as two things at once:
* an **R&D bench** — switch detection/recognition models live, with a full on‑screen telemetry dashboard and serial CSV logging, to compare speed/accuracy on real hardware; and
* an **attendance kiosk** — recognize → "punch" card (snapshot + ID + UTC time), with distance/positioning guidance.

---

## Features

| Area | What |
|---|---|
| Detection | `HumanFaceDetect` — **MSRMNP** (default, 5 landmarks) / **ESPDet224** / **ESPDet416**, switchable live |
| Recognition | `HumanFaceRecognizer` — **MFN** (MobileFaceNet, default) / **MBF**, switchable live, **per‑model DB** |
| Enrollment | ENROLL button → registers the largest centred face; persists in `/spiflash/face_<model>.db` |
| Dashboard | top‑right panel: per‑stage latency + Hz, per‑core compute load, memory, DB capacity, models+metrics, light, position |
| Attendance punch | on a fresh accepted match → centred card: snapshot + `ID n` + `sim` + **UTC** time + distance; serial `PUNCH,` log |
| Distance guide | est. distance (mm) from inter‑pupil distance, with `move BACK / come CLOSER` guidance |
| Anti‑spoof | basic heuristic liveness (Off / Texture / Tex+Motion) — advisory, **not** ML‑grade (no liveness model in esp‑dl) |
| Glare/exposure | warning banner when the scene is too bright / has glare |
| Range modes | centred 4:3 ROI crop fed to the detector (Full / Wide / Med / Tight) to trade FOV for range |
| Display mirror | left↔right flip so the person's left shows on screen‑left (`DISPLAY_MIRROR_X`) |
| Verification | `VERIFY,` serial log of every recognition's raw cosine + decision → measure TAR/FAR on your faces |

On‑screen buttons (bottom): **ENROLL · CLEAR DB · RANGE · DET · REC · SPF**.

---

## Hardware

| Part | Detail |
|---|---|
| SoC | ESP32‑P4 (no NPU; dual RISC‑V @360 MHz + SIMD). ESP32‑C6 present for Wi‑Fi, **unused** |
| Camera | SC2336, MIPI‑CSI, SCCB on **I2C1** (SCL=GPIO13, SDA=GPIO12), 1024×600 RGB565 — buffer is **BGR565** |
| LCD | EK79007, MIPI‑DSI 2‑lane @900 Mbps, 1024×600 RGB565, backlight GPIO31 |
| Touch | GT911, **I2C0** (SCL=GPIO46, SDA=GPIO45), RST=GPIO40, INT=GPIO42 |
| Power | LDO3 = 2.5 V (MIPI DPHY), LDO4 = 3.3 V (camera) |
| PSRAM/Flash | 32 MB (200 MHz octal) / 16 MB |

> The 2 MP MIPI‑CSI camera module **must be connected**.

---

## Build & flash

Requires **ESP‑IDF v5.4.4**. ⚠️ **Use an absolute `IDF_PATH`** — a relative path (e.g. running from
inside the project) regenerates a stale bootloader CMake cache and the build fails with *"source does
not match"*. If that happens, delete `build/bootloader` + `build/bootloader-prefix` and rebuild.

```powershell
$env:IDF_PATH="D:\Payroll_FaceDetection\Petpooja_ESP32P4\v5.4.4\esp-idf"
$env:IDF_TOOLS_PATH="C:\Espressif"
$env:IDF_PYTHON_ENV_PATH="C:\Espressif\python_env\idf5.4_py3.11_env"
. "$env:IDF_PATH\export.ps1"
idf.py set-target esp32p4   # first time only
idf.py -p COM12 flash monitor
```

All esp‑dl models are embedded in the app (flash rodata) — no model partition needed. With all 6 models
packed the app is ~9 MB of the 10 MB `factory` partition (~10 % free).

---

## Models & measured performance (ESP32‑P4)

| Detector | Latency | Accuracy (mAP50‑95) | Landmarks → recognition |
|---|---|---|---|
| MSRMNP (default) | ~45 ms | 0.366 | ✅ yes |
| ESPDet224 | ~100 ms | 0.504 | ❌ (recognition auto‑disables) |
| ESPDet416 | ~385 ms | 0.597 | ❌ |

| Recognizer | Latency | Accuracy (TAR@FAR=1e‑4, IJB‑C) | Measured genuine sim (this board) |
|---|---|---|---|
| MFN (default) | ~165 ms | 90.03 % | ~0.80 avg → ~90 % TAR @0.50 |
| MBF | ~335 ms | 93.94 % | ~0.64 avg (thinner margin) |

* **FPS ≈ 9 is display‑bound** (the full‑screen LVGL flush on core 0), *not* AI‑bound — the AI runs in
  parallel on core 1, so changing the detector barely moves FPS.
* The datasheet figures are bare `model.run()`; the latencies above are end‑to‑end (incl. preprocessing/
  alignment), which is why recognition reads higher than the datasheet's model‑only number.
* Counter‑intuitive but measured: running models from **PSRAM XIP is faster** than from flash here
  (200 MHz octal PSRAM out‑bandwidths 80 MHz QIO flash), so `CONFIG_SPIRAM_XIP_FROM_PSRAM=y` is kept on.

**Recognition‑compatibility rule:** only **MSRMNP** emits the 5 landmarks the recognizer needs for
alignment. ESPDet detectors emit none, so recognition/enroll auto‑disable (the dashboard shows `N/A`)
while an ESPDet model is active — switch back to MSRMNP to recognize.

---

## Attendance punch & UTC time

A punch fires on a fresh accepted match (debounced 5 s/ID, blocked on suspected spoof). It shows a card
(snapshot + ID + similarity + UTC + distance) and logs `PUNCH,id,sim,dist,utc` to serial.

> ⚠️ **No real‑time clock on this board.** The P4 has no battery RTC and no NTP (Wi‑Fi would need the C6
> brought up). The clock is seeded at boot from the firmware **build date** (`PUNCH_BASE_EPOCH`), so the
> UTC stamp is plausible and advancing but **not accurate** until you provide a time source — NTP via the
> C6, an external RTC (DS3231), or a one‑off `settimeofday` over serial.

## Distance guide

Estimated from inter‑pupil distance (`dist = K / ipd_px`), EMA‑smoothed with hysteresis. `DIST_K_IPD` is
a **placeholder — calibrate per lens**: stand at a known distance, read `ipd px` on the dashboard POSITION
line, set `DIST_K_IPD = ipd_px × known_mm`, rebuild. The guide direction works even before calibrating.

---

## Tunables (top of `main/face_processor.cpp`)

| Macro | Meaning |
|---|---|
| `DISPLAY_MIRROR_X` | 1 = mirror the shown image (left=left). Display‑only; set 0 to disable |
| `DETECT_SCORE_THR` / `MIN_FACE_WIDTH` | detector score floor / smallest face (px) for recog/enroll |
| `RECOGNIZE_INTERVAL_US` | recognition cadence (~2 Hz); throttled because recog is the slow stage |
| `RECO_ACCEPT_THR` | accept threshold on raw cosine (0.50). Lower → higher TAR, higher FAR |
| `RANGE_MODES[]` | the 4:3 crop sizes for the RANGE button |
| `DIST_K_IPD` / `DIST_OK_MIN/MAX_MM` | distance calibration + the "OK" band |
| `PUNCH_DEBOUNCE_US` / `PUNCH_REQUIRE_DIST_OK` | punch debounce / require good distance (off until calibrated) |
| `CAM_AUTODETECT_PIX` | re‑enable the 4‑way camera pixel‑format calibration (camera buffer is BGR565) |

---

## Serial logging (capture & analyse)

```powershell
idf.py -p COM12 monitor | Tee-Object run.log
findstr "BENCH," run.log > bench.csv     # per‑second pipeline telemetry (latency/Hz/load/mem)
findstr "VERIFY," run.log > verify.csv   # per‑recognition raw cosine + ACCEPT/REJECT (TAR/FAR)
findstr "PUNCH,"  run.log > punch.csv    # attendance events
```

---

## Repository layout

```
human_face_recognition_live/
├── main/                  firmware (see main/README.md)
│   ├── app_main.cpp         bring‑up: power rails, display, UI, camera, face processor
│   ├── face_processor.cpp   AI task, model switching, distance, punch, telemetry  (core logic)
│   ├── ui.cpp               LVGL dashboard, buttons, punch card, warning banner
│   └── *.hpp
├── components/
│   ├── bsp_camera/        SC2336 MIPI‑CSI capture engine        (see its README)
│   ├── bsp_display/       EK79007 LCD + GT911 touch + LVGL      (see its README)
│   └── spiflash_fatfs/    FAT partition for the face DB         (see its README)
└── README.md             (this file)
```

The **YuNet detector port** (a recognition‑compatible detector with better range) lives in
`../../../yunet_port/` — see `yunet_port/README.md`.

---

## Credits

* Models & inference — Espressif **esp‑dl** (`models/human_face_detect`, `models/human_face_recognition`).
* Board bring‑up (camera/LCD/touch pinout & init) — Elecrow CrowPanel Advanced 7" ESP32‑P4 examples (Lesson13 / Lesson09).
