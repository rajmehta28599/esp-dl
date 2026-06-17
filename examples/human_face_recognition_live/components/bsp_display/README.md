# `bsp_display` — EK79007 LCD + GT911 touch + LVGL

Initializes the panel, backlight, capacitive touch, and the LVGL port for the CrowPanel 7" board.
Pin/timing values come from Elecrow's official Lesson07/09/13 BSP.

* **LCD:** EK79007 over **MIPI‑DSI** (2 data lanes @900 Mbps), **1024×600 RGB565**.
* **Backlight:** GPIO31, LEDC PWM @30 kHz (`set_lcd_blight(0..100)`).
* **Touch:** GT911 over **I2C0** (SCL=GPIO46, SDA=GPIO45), RST=GPIO40, INT=GPIO42; registered as an LVGL
  input device so the on‑screen buttons work.
* **LVGL:** via `esp_lvgl_port` — double draw buffer (full‑screen), `sw_rotate`, `full_refresh=false`,
  `avoid_tearing=false`; the port runs the LVGL timer task (5 ms tick).

## API (`bsp_display.h`)

```c
esp_err_t display_init(void);          // backlight + MIPI‑DSI panel + LVGL + GT911 touch
esp_err_t set_lcd_blight(uint32_t pct); // backlight brightness 0..100
extern lv_display_t        *g_lvgl_disp;
extern esp_lcd_touch_handle_t g_touch_handle;
```

## Known quirks (cosmetic, expected)

* GT911 **fails its first init then succeeds on the driver's retry** — the boot log shows one
  `GT911 init failed` line; touch works after the automatic retry.
* `esp_lcd_panel_swap_xy ... not supported by this panel` — harmless; the panel is used unrotated.
* The camera canvas is fed RGB565LE while the sensor buffer is BGR565, so the live image has a cosmetic
  R/B swap. The application optionally mirrors the image horizontally for display (`DISPLAY_MIRROR_X`).
