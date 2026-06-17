# `bsp_camera` — SC2336 MIPI‑CSI capture engine

Brings up the SC2336 camera and streams frames to a user callback. Ported from Elecrow's
`Lesson13‑Camera_Real‑Time` BSP (display/canvas code removed — rendering is the UI layer's job).

* **Sensor:** SC2336, MIPI‑CSI, configured (via sdkconfig) to **1024×600 @30 fps**.
* **SCCB (sensor control):** I2C port **1**, SCL=GPIO13, SDA=GPIO12, 100 kHz.
* **Pipeline:** `esp_video` / V4L2 → ISP converts RAW8 → **RGB565** (note: the delivered buffer is
  **BGR565** byte order in practice — see `main/README.md`).
* **Buffers:** two PSRAM `USERPTR` frame buffers (`CAM_H_RES × CAM_V_RES × 2`), cache‑aligned.
* The capture task runs the per‑frame callback **on the core passed to `camera_start()`** (core 0 here).

## API (`bsp_camera.h`)

```c
esp_err_t camera_video_init(void);                                   // SCCB bus + esp_video (CSI)
esp_err_t video_register_frame_operation_cb(camera_video_frame_operation_cb_t cb); // before start
int       camera_start(int core_id);                                 // open device, alloc bufs, run task
void      set_camera_img_display(bool state);                        // enable/disable frame delivery
```

The callback signature is `void cb(uint8_t *buf, uint8_t idx, uint32_t w, uint32_t h, size_t len)`; `buf`
is a live V4L2 buffer (valid until the callback returns). `CAM_H_RES`/`CAM_V_RES` (1024/600) must match the
sdkconfig SC2336 format or `video_open()` aborts with a resolution‑mismatch error.
