#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Build the LVGL widgets: full-screen camera canvas, status bar, and the
// "Enroll" / "Clear DB" touch buttons. Must be called after display_init().
void ui_init(void);

// Point the camera canvas at a freshly captured (and overlay-annotated) RGB565
// frame and repaint. Safe to call from the camera capture task; takes the LVGL lock.
void ui_update_camera_canvas(uint8_t *buf, uint32_t w, uint32_t h);

// Update the top status bar text (faces / recognition / fps). Thread-safe.
void ui_set_status(const char *text);

// Update the enrollment status line. Thread-safe.
void ui_set_enroll_status(const char *text);

#ifdef __cplusplus
}
#endif
