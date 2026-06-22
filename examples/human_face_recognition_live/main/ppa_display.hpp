#pragma once
#include <stdint.h>
#include "esp_err.h"

/*
 * Increment-1 PPA coexistence spike (TEST_LOG 015).
 *
 * Proves the FAST display path: the camera is blitted by the PPA hardware accelerator
 * DIRECTLY into the DSI panel framebuffer (byte-swap + R/B swap in silicon), while LVGL
 * stays the SOLE caller of esp_lcd_panel_draw_bitmap for chrome.
 *
 * Why direct-to-FB and NOT esp_lcd_panel_draw_bitmap: dpi_panel_draw_bitmap fires the
 * panel's on_color_trans_done callback, which esp_lvgl_port owns to drive its buffer-swap
 * state machine. A 2nd draw_bitmap caller would desync LVGL (hang / chrome corruption), and
 * lvgl_port_lock() does NOT guard that. Writing the camera region straight into the FB (the
 * FB is scanned out continuously at num_fbs=1) needs no draw_bitmap, so LVGL is undisturbed.
 *
 * SPIKE LAYOUT (non-overlapping, the only thing this increment proves):
 *   - PPA owns the TOP camera region (rows 0..PPA_CAM_H-1).
 *   - LVGL owns the BOTTOM chrome strip (rows PPA_CAM_H..V_RES-1).
 *   Chrome OVERLAYING the live camera (transparent labels on top) is a LATER increment.
 *
 * Set USE_PPA_DISPLAY 0 to restore the keeper LVGL-canvas path (v3.3.5-37).
 */
#define USE_PPA_DISPLAY 1

// Camera region inside the 1024x600 panel framebuffer. Top area; chrome strip is below it.
#define PPA_CAM_X 0
#define PPA_CAM_Y 0
#define PPA_CAM_W 1024
#define PPA_CAM_H 480 // top 480 of 600 rows; bottom 120 px = LVGL chrome strip

#ifdef __cplusplus
extern "C" {
#endif

// Register the PPA SRM client and grab the DSI panel framebuffer. Call once, after display_init().
esp_err_t ppa_display_init(void);

// Blit one camera frame (cam_w x cam_h, RGB565-layout / BGR565 from the SC2336) into the panel
// FB camera region via PPA (byte-swap + R/B swap in hardware). No draw_bitmap, no LVGL contention.
void ppa_display_blit(const uint8_t *cam_buf, uint32_t cam_w, uint32_t cam_h);

#ifdef __cplusplus
}
#endif
