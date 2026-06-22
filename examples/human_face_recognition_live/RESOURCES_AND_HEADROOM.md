# ESP32-P4 Resources, Utilization & Headroom — Face Attendance

_2026-06-22. Measured on this board (CrowPanel 7" P4, 32 MB octal PSRAM, 16 MB flash, SC2336) at the current
production build: PPA full UI, YuNet256 + MFN, 30 fps. Numbers from boot log + live `BENCH` + build size +
sdkconfig._

---

## 1. Processing units on the chip

| Unit | Spec | This build | Used for | Headroom |
|---|---|---|---|---|
| HP CPU core 0 (RISC-V RV32 + 128-bit AI/DSP vector ISA) | up to **400 MHz** | **360 MHz** | display blit trigger + camera capture + LVGL chrome | **~70–85% idle** |
| HP CPU core 1 (same) | up to 400 MHz | 360 MHz | AI: detection + recognition (esp-dl, int8 on the vector ISA) | ~10–50% idle @256 |
| LP CPU core (RISC-V) | 40 MHz | running | system | **unused by app** |
| **AI compute** | the HP cores' **vector/AI ISA** (NO separate NPU on P4) | yes | esp-dl int8 conv | (it IS the cores above) |
| **PPA** (Pixel-Processing Accelerator) | scale/rotate/mirror/blend | yes | camera→FB blit (30 fps) | spare (one blit/frame) |
| ISP (image signal processor) | AE/AWB/demosaic | yes | camera pipeline | — |
| 2D-DMA | — | yes (under PPA) | — | — |
| **HW JPEG codec** | encode/decode | **unused** | — | **free capability** |
| **HW H.264 encoder** | — | **unused** | — | **free capability** |
| MIPI-CSI / MIPI-DSI | camera / display | yes | SC2336 in, EK79007 out | — |
| Crypto (AES/SHA/RSA/HMAC) | — | unused | — | free (for secure backend) |
| USB-OTG HS | — | unused | — | free |

## 2. Memory units

| Memory | Total | Used (runtime) | Free | %used | Notes |
|---|---|---|---|---|---|
| Internal SRAM (HP L2MEM) | **768 KB** | ~520 KB | **~247 KB free heap** | ~68% | static + stacks + DMA + heap; the scarce fast RAM |
| TCM (tightly-coupled) | 8 KB | ~1 KB | ~7 KB | — | fastest, tiny |
| PSRAM (octal @ 200 MHz) | **32 MB** (heap pool ~23.5 MB) | ~13 MB | **~10.4 MB free** | ~55% of heap | camera 3×1.2 MB ring + FB 1.2 MB + models (XIP) + LVGL + DB |
| Flash (app partition) | **10 MB** | **9.57 MB** | **0.43 MB** | **96% (TIGHT)** | 4 embedded YuNet models ≈ 0.6 MB |
| Flash (storage / FAT) | 1 MB | (face DBs) | — | — | per-detector person DBs |
| Flash (unpartitioned) | ~5 MB | — | ~5 MB | — | room to grow app or storage |

**Key architectural fact:** `SPIRAM_XIP_FROM_PSRAM` — code + rodata run **from PSRAM**, so CPU instruction
fetch shares the PSRAM bus with PPA + camera + AI tensors. PSRAM bandwidth is the true shared resource (this is
why the PPA display win also cut AI latency ~1.5–2×: removing the LVGL flush de-congested PSRAM).

## 3. Utilization summary (30 fps, YuNet256 + MFN)

- **CPU:** core 0 **~15–30%**, core 1 **~50–89%** (bursts to 90%+ at the AI cycle). → core 0 has big spare,
  core 1 moderate spare. At YuNet512 core 1 hits ~96% (near saturation) but fps still 30.
- **Internal RAM:** ~247 KB free of 768 KB → healthy.
- **PSRAM:** ~10.4 MB free of ~23.5 MB → healthy.
- **Flash app:** 96% full → **the binding constraint** (4 YuNet models). Drop unused resolutions (keep 256) to
  reclaim ~0.45 MB, or enlarge the app partition into the ~5 MB unpartitioned space.
- **FPS:** pinned at **30 = the SC2336 sensor cap** (panel tops ~56 Hz); display path is no longer the limit.

## 4. "Can we make it more powerful?" — honest levers

**FPS is maxed** (sensor 30 / panel 56 Hz). You cannot make the *preview* faster without a higher-fps sensor
mode. But there is real **compute + memory headroom** to do MORE per frame, or to save power:

| Lever | Gain | Cost / caveat |
|---|---|---|
| CPU 360 → **400 MHz** | ~+11% compute (less if PSRAM-fetch-bound via XIP) | more power/heat — bad for a battery device |
| Use **idle core 0 for AI** (run detection on core 0, recognition on core 1 → pipeline) | faster recognition cadence (~6 → ~10/s) | real refactor; core 0 also does the blit |
| **Liveness / anti-spoof** model | security (defeats photo/video) | fits the spare core-1 + PSRAM budget |
| **Multi-face** simultaneous recognition | recognize 2–3 people at once | uses spare cycles |
| **MBF** recognizer (vs MFN) | wider margins / accuracy | ~2× rec latency — affordable on PPA |
| Move hot code to **IRAM** (internal SRAM) | fewer PSRAM-fetch stalls → faster | limited internal RAM |
| **HW JPEG / H.264** | snapshot/clip per punch to backend | a feature, not speed |
| **Down-clock / sleep between frames** | LOWER power (cores idle ~70%) | the *opposite* of "more powerful" — but right for a compact battery device |

**Recommendation for a compact, battery/thermal-sensitive attendance device:** don't chase more raw power —
fps is capped and the cores already idle most of the time. Spend the headroom on **capability** (liveness,
multi-face, MBF accuracy) and, if it's battery-powered, consider **down-clocking/sleeping to extend runtime**.
The one real constraint to manage is the **96%-full app partition** (trim to one YuNet model for production).

## 5. Complete benchmark — measured vs theory (the whole journey)

### 5.1 Frame rate
| Build | FPS | Theory | Match |
|---|---|---|---|
| LVGL keeper | ~9 | core-0 display-flush bound (~70 ms/frame) | ✓ |
| det-throttle + mirror-off | ~9 | freed core 1; still display-capped | ✓ |
| PPA blocking | 20 | 22 ms blit blocks core 0; 22+10 > 33 ms | ✓ |
| **PPA non-blocking** | **30** | blit overlaps capture → sensor cap 33.3 ms | ✓ |

### 5.2 Detection latency (PPA path vs old LVGL-display-on vs esp-dl datasheet)
| Detector | input | px | det ms (PPA) | det ms (LVGL on) | datasheet bare | theory |
|---|---|---|---|---|---|---|
| MSRMNP | 160×120 | 19k | ~18 | ~44 | 17 | PPA ≈ bare (PSRAM freed) ✓ |
| YuNet128 | 128×96 | 12k | ~15 | — | — | 0.25× of 256 → ~15 ✓ |
| YuNet256 | 256×192 | 49k | ~60 | ~92 | — | baseline; PPA 1.5× faster than LVGL ✓ |
| YuNet384 | 384×288 | 110k | ~150 | ~235 | — | 2.25× of 256 → ~135 (≈, +overhead) ✓ |
| YuNet512 | 512×384 | 196k | ~290 | — | — | 4× of 256 → ~240 (+overhead) ✓ |
| ESPDet224 | 224×224 | 50k | ~67 | ~112 | 52 | ✓ |
| ESPDet416 | 416×416 | 173k | ~270 | ~380 | 193 | ✓ |

**det ∝ input pixels + fixed overhead** — reproduced across a 16× pixel swing. **PPA path runs detection
~1.5–2× faster than the old display-on path** (PSRAM de-congestion — the XIP/contention theory, confirmed).

### 5.3 Recognition latency
| Recognizer | feat | rec ms (PPA) | rec ms (LVGL era) | datasheet bare | theory |
|---|---|---|---|---|---|
| MFN | 512 | ~106–118 | ~165 | 96 | end-to-end (align+warp+run+match); PPA faster ✓ |
| MBF | 512 | (~210 est) | ~330 | 191 | ~2× MFN (datasheet 1:1.99) ✓ |

### 5.4 Recognition accuracy (YuNet256 + MFN)
| Condition | genuine sim | cross `2nd` | theory |
|---|---|---|---|
| close (~450 mm) | 0.91–0.96 | 0.17–0.39 | strongly discriminative ✓ |
| ~1 m | 0.79–0.86 | — | falls with distance (fewer px) but clears 0.62 ✓ |
| enrollment quality | dominates accuracy | — | bad enroll → ~0.34 (matches prior bench) ✓ |

### 5.5 Memory over the run
| Resource | value | theory |
|---|---|---|
| Internal free | ~247 KB (YuNet), 263 KB (MSRMNP) | drops with model set, no leak ✓ |
| PSRAM free | ~10.4 MB (YuNet256, +3-buf ring) | scales with model + buffers ✓ |
| Flash app | 9.57 MB / 10 MB | 4 models embedded ✓ |

**Scorecard:** detector ordering, pixel-scaling, MFN:MBF ≈ 1:2, display-bound→PPA-unbound fps, PSRAM-XIP
contention (AI sped up by the display fix), recognition discriminative + enrollment-dominated, no memory leak —
**all reproduce theory.**
