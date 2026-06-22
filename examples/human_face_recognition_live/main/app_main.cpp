/*
 * Live face detection + recognition demo
 * Board: Elecrow CrowPanel Advanced 7" ESP32-P4 HMI AI Display (1024x600)
 *
 * Pipeline:
 *   SC2336 (MIPI-CSI) --esp_video/ISP--> RGB565 frame
 *     -> AI task (core 1): HumanFaceDetect + HumanFaceRecognizer  (esp-dl)
 *     -> overlay boxes/keypoints/labels, render via LVGL canvas on the EK79007 LCD
 *   GT911 touch -> "ENROLL" / "CLEAR DB" buttons
 *
 * The face feature database is stored in the on-flash "storage" FAT partition.
 */
#include "esp_log.h"
#include "esp_err.h"
#include "esp_ldo_regulator.h"
#include "driver/gpio.h"

#include "spiflash_fatfs.hpp"
#include "bsp_display.h"
#include "bsp_camera.h"
#include "ui.hpp"
#include "face_processor.hpp"
#include "ppa_display.hpp"

#include <string>

static const char *TAG = "main";

static esp_ldo_channel_handle_t s_ldo3 = nullptr; // 2.5V - MIPI DPHY
static esp_ldo_channel_handle_t s_ldo4 = nullptr; // 3.3V - camera / peripherals

static void fatal(const char *what, esp_err_t err)
{
    ESP_LOGE(TAG, "%s failed: %s", what, esp_err_to_name(err));
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

static esp_err_t power_rails_init(void)
{
    esp_ldo_channel_config_t ldo3_cfg = {.chan_id = 3, .voltage_mv = 2500};
    esp_err_t err = esp_ldo_acquire_channel(&ldo3_cfg, &s_ldo3);
    if (err != ESP_OK) {
        return err;
    }
    esp_ldo_channel_config_t ldo4_cfg = {.chan_id = 4, .voltage_mv = 3300};
    return esp_ldo_acquire_channel(&ldo4_cfg, &s_ldo4);
}

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "==== CrowPanel P4 live face recognition demo ====");

    // 1. Mount the FAT partition that stores the face feature database.
        ESP_ERROR_CHECK(fatfs_flash_mount());
    std::string db_path = std::string(CONFIG_SPIFLASH_MOUNT_POINT) + "/face.db";

    // 2. Power the MIPI DPHY (LDO3, 2.5V) and camera/peripheral rail (LDO4, 3.3V).
    esp_err_t err = power_rails_init();
    if (err != ESP_OK) {
        fatal("LDO power", err);
    }

    // 3. GPIO ISR service (shared by GT911 touch INT and the camera driver).
    err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        fatal("gpio isr service", err);
    }

    // 4. LCD + touch + LVGL.
    err = display_init();
    if (err != ESP_OK) {
        fatal("display", err);
    }

    // 5. Build the on-screen UI.
    ui_init();
#if USE_PPA_DISPLAY
    err = ppa_display_init(); // register PPA SRM client + grab the DSI framebuffer (after panel up)
    if (err != ESP_OK) {
        fatal("ppa display", err);
    }
#endif
    err = set_lcd_blight(100);
    if (err != ESP_OK) {
        fatal("backlight", err);
    }

    // 6. Create detector + recognizer + AI task. Registers the camera frame callback.
    err = face_processor_init(db_path.c_str());
    if (err != ESP_OK) {
        fatal("face processor", err);
    }

    // 7. Bring up the SC2336 camera and start streaming on core 0.
    err = camera_video_init();
    if (err != ESP_OK) {
        fatal("camera init", err);
    }
    if (camera_start(0) < 0) {
        fatal("camera start", ESP_FAIL);
    }
    set_camera_img_display(true);

    ESP_LOGI(TAG, "Running. Point the camera at a face; tap ENROLL to register it.");
}
