#include "ppa_display.hpp"

#include "bsp_display.h" // panel_handle, BSP_LCD_H_RES/V_RES, esp_lcd_dpi_panel_get_frame_buffer
#include "driver/ppa.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "ppa_disp";

static ppa_client_handle_t s_srm = nullptr;
static uint8_t *s_fb = nullptr; // the DSI panel framebuffer (full screen RGB565, PSRAM/DMA)
static size_t s_fb_size = 0;
static SemaphoreHandle_t s_done = nullptr; // given when a PPA blit completes; serializes submissions
static volatile bool s_paused = false;     // when set, skip blits so LVGL can own the whole FB

// PPA completion callback — runs in ISR context. Returns whether a higher-priority task was woken so
// the driver performs the yield. Releases s_done so the next frame's blit may submit.
static bool IRAM_ATTR ppa_on_done(ppa_client_handle_t client, ppa_event_data_t *edata, void *user)
{
    (void)client;
    (void)edata;
    (void)user;
    BaseType_t hpw = pdFALSE;
    xSemaphoreGiveFromISR(s_done, &hpw);
    return hpw == pdTRUE;
}

esp_err_t ppa_display_init(void)
{
    // panel_handle IS a dpi-panel handle (esp_lvgl_port already calls esp_lcd_dpi_panel_* on it),
    // so get_frame_buffer works. num_fbs=1 -> one FB, scanned out continuously.
    esp_err_t err = esp_lcd_dpi_panel_get_frame_buffer(panel_handle, 1, (void **)&s_fb);
    if (err != ESP_OK || !s_fb) {
        ESP_LOGE(TAG, "get_frame_buffer failed: %s", esp_err_to_name(err));
        return err != ESP_OK ? err : ESP_FAIL;
    }
    s_fb_size = (size_t)BSP_LCD_H_RES * BSP_LCD_V_RES * 2;

    ppa_client_config_t cfg = {};
    cfg.oper_type = PPA_OPERATION_SRM;
    cfg.max_pending_trans_num = 1;
    err = ppa_register_client(&cfg, &s_srm);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ppa_register_client failed: %s", esp_err_to_name(err));
        return err;
    }

    // Completion callback + a binary semaphore serialize submissions and let the ~22 ms blit run
    // concurrently with the next capture (non-blocking). Start the semaphore AVAILABLE (first submit).
    ppa_event_callbacks_t cbs = {};
    cbs.on_trans_done = ppa_on_done;
    err = ppa_client_register_event_callbacks(s_srm, &cbs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ppa register callbacks failed: %s", esp_err_to_name(err));
        return err;
    }
    s_done = xSemaphoreCreateBinary();
    if (!s_done) {
        ESP_LOGE(TAG, "no mem for PPA done semaphore");
        return ESP_ERR_NO_MEM;
    }
    xSemaphoreGive(s_done);

    ESP_LOGI(TAG, "PPA display ready: fb=%p (%u B), cam region %dx%d @ (%d,%d)", s_fb,
             (unsigned)s_fb_size, PPA_CAM_W, PPA_CAM_H, PPA_CAM_X, PPA_CAM_Y);
    return ESP_OK;
}

void ppa_display_blit(const uint8_t *cam_buf, uint32_t cam_w, uint32_t cam_h)
{
    if (!s_srm || !s_fb || !cam_buf || s_paused) {
        return; // paused -> LVGL owns the whole FB (modal overlay); skip without touching s_done
    }
    // Wait for the PREVIOUS blit to finish — it ran concurrently with this frame's capture, so it is
    // normally already done and this returns immediately (core 0 is not stalled). The 3-deep camera
    // ring keeps cam_buf valid until long after the previous blit completed.
    if (s_done && xSemaphoreTake(s_done, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGW(TAG, "previous PPA blit did not finish in 100 ms; dropping frame");
        return;
    }
    if (s_paused) { // pause raised during the wait -> hand the FB to LVGL, do not submit
        xSemaphoreGive(s_done);
        return;
    }

    // Crop the top PPA_CAM_H rows of the camera (scale 1:1) into the FB camera region (byte_swap=0,
    // rgb_swap=0 = identity copy + correct color, Test 016). Submitted NON-BLOCKING so the ~22 ms op
    // overlaps the next capture; ppa_on_done releases s_done on completion.
    ppa_srm_oper_config_t srm = {};
    srm.in.buffer = (void *)cam_buf;
    srm.in.pic_w = cam_w;
    srm.in.pic_h = cam_h;
    srm.in.block_w = PPA_CAM_W;
    srm.in.block_h = PPA_CAM_H;
    srm.in.block_offset_x = 0;
    srm.in.block_offset_y = 0;
    srm.in.srm_cm = PPA_SRM_COLOR_MODE_RGB565;

    srm.out.buffer = s_fb;
    srm.out.buffer_size = s_fb_size;
    srm.out.pic_w = BSP_LCD_H_RES; // full FB width = stride
    srm.out.pic_h = BSP_LCD_V_RES;
    srm.out.block_offset_x = PPA_CAM_X;
    srm.out.block_offset_y = PPA_CAM_Y;
    srm.out.srm_cm = PPA_SRM_COLOR_MODE_RGB565;

    srm.rotation_angle = PPA_SRM_ROTATION_ANGLE_0;
    srm.scale_x = 1.0f;
    srm.scale_y = 1.0f;
    srm.rgb_swap = 0;  // Test 016: (byte_swap=0, rgb_swap=0) renders CORRECT color on the EK79007 FB
    srm.byte_swap = 0; // (also fixes the R/B swap the old LVGL path always had)
    srm.mode = PPA_TRANS_MODE_NON_BLOCKING;

    esp_err_t err = ppa_do_scale_rotate_mirror(s_srm, &srm);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ppa srm submit failed: %s", esp_err_to_name(err));
        if (s_done) {
            xSemaphoreGive(s_done); // nothing queued -> on_trans_done won't fire; release for next frame
        }
    }
    // PPA writes the FB via DMA (concurrently with capture); the DSI scans it via DMA.
}

void ppa_display_pause(void)
{
    s_paused = true; // new blits skip immediately (they check s_paused before taking s_done)
    // Drain a possibly in-flight blit: wait for its completion callback to give s_done, then release.
    // After this returns, no PPA DMA is touching the FB, so the LVGL task may safely render over it.
    if (s_done && xSemaphoreTake(s_done, pdMS_TO_TICKS(200)) == pdTRUE) {
        xSemaphoreGive(s_done);
    }
}

void ppa_display_resume(void)
{
    s_paused = false;
}
