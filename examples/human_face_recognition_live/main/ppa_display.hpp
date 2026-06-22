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

// Camera region inside the 1024x600 panel framebuffer. Top area; the bottom band is the LVGL
// chrome (buttons + status), which never overlaps this rect so PPA owns it uncontested.
#define PPA_CAM_X 0
#define PPA_CAM_Y 0
#define PPA_CAM_W 1024
#define PPA_CAM_H 440 // top 440 of 600 rows; bottom 160 px = LVGL chrome band (2 button rows + status)

#ifdef __cplusplus
extern "C" {
#endif

// Register the PPA SRM client and grab the DSI panel framebuffer. Call once, after display_init().
esp_err_t ppa_display_init(void);

// Blit one camera frame (cam_w x cam_h, RGB565-layout / BGR565 from the SC2336) into the panel
// FB camera region via PPA (byte-swap + R/B swap in hardware). No draw_bitmap, no LVGL contention.
void ppa_display_blit(const uint8_t *cam_buf, uint32_t cam_w, uint32_t cam_h);

// Pause/resume the camera blits so LVGL can render a full-screen modal (punch card, menu) over the
// whole framebuffer. ppa_display_pause() DRAINS any in-flight blit (waits on the completion semaphore)
// before returning, so LVGL never writes the FB while a PPA DMA is mid-flight (cross-core tear-safe).
// Call pause() from the LVGL task before showing the overlay, resume() after hiding it.
void ppa_display_pause(void);
void ppa_display_resume(void);

#ifdef __cplusplus
}
#endif
