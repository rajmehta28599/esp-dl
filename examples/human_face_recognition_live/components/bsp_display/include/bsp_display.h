#ifndef _BSP_DISPLAY_H_
#define _BSP_DISPLAY_H_
/*
 * Display + touch BSP for the Elecrow CrowPanel Advanced 7" ESP32-P4 board.
 *   - LCD:   EK79007 over MIPI-DSI (2 data lanes), 1024x600, RGB565
 *   - Touch: GT911 over I2C0 (SCL=46, SDA=45), RST=40, INT=42
 *   - Backlight: GPIO31, LEDC PWM
 *   - Renders through LVGL (esp_lvgl_port). Touch is registered as an LVGL input
 *     device so on-screen buttons work.
 *
 * Pin / timing values are taken from Elecrow's official Lesson07/09/13 BSP for
 * this exact board.
 */
#include "esp_log.h"
#include "esp_err.h"
#include "esp_lcd_ek79007.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_lvgl_port.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/i2c_master.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DISPLAY_TAG "DISPLAY"
#define DISPLAY_INFO(fmt, ...) ESP_LOGI(DISPLAY_TAG, fmt, ##__VA_ARGS__)
#define DISPLAY_DEBUG(fmt, ...) ESP_LOGD(DISPLAY_TAG, fmt, ##__VA_ARGS__)
#define DISPLAY_ERROR(fmt, ...) ESP_LOGE(DISPLAY_TAG, fmt, ##__VA_ARGS__)

#define BSP_LCD_H_RES 1024 // Horizontal resolution
#define BSP_LCD_V_RES 600  // Vertical resolution
#define BSP_LCD_BITS_PER_PIXEL 16

#define LCD_GPIO_BLIGHT 31  // LCD backlight GPIO
#define BLIGHT_PWM_HZ 30000 // LCD backlight PWM frequency

// GT911 capacitive touch (I2C port 0)
#define TOUCH_I2C_PORT 0
#define TOUCH_GPIO_SCL 46
#define TOUCH_GPIO_SDA 45
#define TOUCH_GPIO_RST 40
#define TOUCH_GPIO_INT 42

extern lv_display_t *g_lvgl_disp;        // LVGL display handle
extern esp_lcd_touch_handle_t g_touch_handle;

// Initialize backlight + MIPI-DSI panel + LVGL + GT911 touch (registered to LVGL).
esp_err_t display_init(void);

// Set backlight brightness (0 - 100).
esp_err_t set_lcd_blight(uint32_t brightness);

#ifdef __cplusplus
}
#endif
#endif // _BSP_DISPLAY_H_
