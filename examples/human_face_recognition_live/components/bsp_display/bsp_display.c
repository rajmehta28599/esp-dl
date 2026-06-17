#include "bsp_display.h"

/* ---------------- module state ---------------- */
esp_lcd_panel_handle_t panel_handle = NULL;
lv_display_t *g_lvgl_disp = NULL;
esp_lcd_touch_handle_t g_touch_handle = NULL;

static esp_lcd_dsi_bus_handle_t mipi_dsi_bus = NULL;
static esp_lcd_panel_io_handle_t mipi_dbi_io = NULL;
static i2c_master_bus_handle_t touch_i2c_bus = NULL;
static esp_lcd_panel_io_handle_t tp_io_handle = NULL;

/* ---------------- backlight (LEDC PWM) ---------------- */
static esp_err_t blight_init(void)
{
    esp_err_t err = ESP_OK;
    const gpio_config_t gpio_cfg = {
        .pin_bit_mask = (1ULL << LCD_GPIO_BLIGHT),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = false,
        .pull_down_en = false,
        .intr_type = GPIO_INTR_DISABLE,
    };
    err = gpio_config(&gpio_cfg);
    if (err != ESP_OK)
        return err;

    const ledc_timer_config_t timer_config = {
        .clk_cfg = LEDC_USE_PLL_DIV_CLK,
        .duty_resolution = LEDC_TIMER_11_BIT,
        .freq_hz = BLIGHT_PWM_HZ,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .timer_num = LEDC_TIMER_0,
    };
    const ledc_channel_config_t channel_config = {
        .gpio_num = LCD_GPIO_BLIGHT,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
        .hpoint = 0,
    };
    err = ledc_timer_config(&timer_config);
    if (err != ESP_OK)
        return err;
    return ledc_channel_config(&channel_config);
}

/* brightness: 0 - 100 */
esp_err_t set_lcd_blight(uint32_t brightness)
{
    esp_err_t err = ESP_OK;
    uint32_t duty = (brightness != 0) ? ((brightness * 18) + 200) : 0;
    err = ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
    if (err != ESP_OK)
        return err;
    return ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

/* ---------------- MIPI-DSI EK79007 panel ---------------- */
static esp_err_t display_port_init(void)
{
    esp_err_t err = ESP_OK;
    lcd_color_rgb_pixel_format_t dpi_pixel_format = LCD_COLOR_PIXEL_FORMAT_RGB565;

    esp_lcd_dsi_bus_config_t bus_config = {
        .bus_id = 0,
        .num_data_lanes = 2,
        .phy_clk_src = MIPI_DSI_PHY_CLK_SRC_DEFAULT,
        .lane_bit_rate_mbps = 900,
    };
    err = esp_lcd_new_dsi_bus(&bus_config, &mipi_dsi_bus);
    if (err != ESP_OK)
        return err;

    esp_lcd_dbi_io_config_t dbi_config = {
        .virtual_channel = 0,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    err = esp_lcd_new_panel_io_dbi(mipi_dsi_bus, &dbi_config, &mipi_dbi_io);
    if (err != ESP_OK)
        return err;

    const esp_lcd_dpi_panel_config_t dpi_config = {
        .dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
        .dpi_clock_freq_mhz = 51,
        .virtual_channel = 0,
        .pixel_format = dpi_pixel_format,
        .num_fbs = 1,
        .video_timing = {
            .h_size = BSP_LCD_H_RES,
            .v_size = BSP_LCD_V_RES,
            .hsync_back_porch = 160,
            .hsync_pulse_width = 70,
            .hsync_front_porch = 160,
            .vsync_back_porch = 23,
            .vsync_pulse_width = 10,
            .vsync_front_porch = 12,
        },
        .flags.use_dma2d = true,
    };

    ek79007_vendor_config_t vendor_config = {
        .mipi_config = {
            .dsi_bus = mipi_dsi_bus,
            .dpi_config = &dpi_config,
        },
    };
    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = -1,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = BSP_LCD_BITS_PER_PIXEL,
        .vendor_config = &vendor_config,
    };
    err = esp_lcd_new_panel_ek79007(mipi_dbi_io, &panel_config, &panel_handle);
    if (err != ESP_OK)
        return err;
    err = esp_lcd_panel_reset(panel_handle);
    if (err != ESP_OK)
        return err;
    return esp_lcd_panel_init(panel_handle);
}

/* ---------------- GT911 touch (I2C0) ---------------- */
static esp_err_t touch_init(void)
{
    esp_err_t err = ESP_OK;

    i2c_master_bus_config_t i2c_conf = {
        .i2c_port = TOUCH_I2C_PORT,
        .sda_io_num = TOUCH_GPIO_SDA,
        .scl_io_num = TOUCH_GPIO_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    err = i2c_new_master_bus(&i2c_conf, &touch_i2c_bus);
    if (err != ESP_OK)
        return err;

    esp_lcd_panel_io_i2c_config_t io_config = {
        .dev_addr = ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS,
        .control_phase_bytes = 1,
        .dc_bit_offset = 0,
        .lcd_cmd_bits = 16,
        .flags = {
            .disable_control_phase = 1,
        },
        .scl_speed_hz = 400000,
    };
    esp_lcd_touch_config_t tp_cfg = {
        .x_max = BSP_LCD_H_RES,
        .y_max = BSP_LCD_V_RES,
        .rst_gpio_num = TOUCH_GPIO_RST,
        .int_gpio_num = TOUCH_GPIO_INT,
        .levels = {
            .reset = 0,
            .interrupt = 0,
        },
        .flags = {
            .swap_xy = false,
            .mirror_x = false,
            .mirror_y = false,
        },
    };

    err = esp_lcd_new_panel_io_i2c(touch_i2c_bus, &io_config, &tp_io_handle);
    if (err != ESP_OK)
        return err;
    err = esp_lcd_touch_new_i2c_gt911(tp_io_handle, &tp_cfg, &g_touch_handle);
    if (err != ESP_OK) {
        // Some panels answer on the backup I2C address.
        io_config.dev_addr = ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS_BACKUP;
        err = esp_lcd_new_panel_io_i2c(touch_i2c_bus, &io_config, &tp_io_handle);
        if (err != ESP_OK)
            return err;
        err = esp_lcd_touch_new_i2c_gt911(tp_io_handle, &tp_cfg, &g_touch_handle);
    }
    return err;
}

/* ---------------- LVGL ---------------- */
static esp_err_t lvgl_init(void)
{
    const lvgl_port_cfg_t lvgl_cfg = {
        .task_priority = configMAX_PRIORITIES - 4,
        .task_stack = 8192 * 2,
        .task_affinity = -1,
        .task_max_sleep_ms = 10,
        .timer_period_ms = 5,
    };
    if (lvgl_port_init(&lvgl_cfg) != ESP_OK) {
        DISPLAY_ERROR("LVGL port init failed");
        return ESP_FAIL;
    }

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = mipi_dbi_io,
        .panel_handle = panel_handle,
        .control_handle = panel_handle,
        .buffer_size = (BSP_LCD_H_RES * BSP_LCD_V_RES * ((BSP_LCD_BITS_PER_PIXEL + 7) / 8)),
        .double_buffer = true,
        .hres = BSP_LCD_H_RES,
        .vres = BSP_LCD_V_RES,
        .monochrome = false,
#if LVGL_VERSION_MAJOR >= 9
        .color_format = LV_COLOR_FORMAT_RGB565,
#endif
        .rotation = {
            .swap_xy = false,
            .mirror_x = false,
            .mirror_y = false,
        },
        .flags = {
            .buff_dma = false,
            .buff_spiram = true,
            .sw_rotate = true,
#if LVGL_VERSION_MAJOR >= 9
            .swap_bytes = true,
#endif
            .full_refresh = false,
            .direct_mode = false,
        },
    };
    const lvgl_port_display_dsi_cfg_t lvgl_dpi_cfg = {
        .flags = {
            .avoid_tearing = false,
        },
    };
    g_lvgl_disp = lvgl_port_add_disp_dsi(&disp_cfg, &lvgl_dpi_cfg);
    if (g_lvgl_disp == NULL) {
        DISPLAY_ERROR("LVGL dsi port add fail");
        return ESP_FAIL;
    }

    // Register GT911 touch as an LVGL input device so buttons receive presses.
    const lvgl_port_touch_cfg_t touch_cfg = {
        .disp = g_lvgl_disp,
        .handle = g_touch_handle,
    };
    if (lvgl_port_add_touch(&touch_cfg) == NULL) {
        DISPLAY_ERROR("LVGL touch port add fail");
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t display_init(void)
{
    esp_err_t err = ESP_OK;
    err = blight_init();
    if (err != ESP_OK)
        return err;
    err = display_port_init();
    if (err != ESP_OK)
        return err;
    err = touch_init();
    if (err != ESP_OK) {
        DISPLAY_ERROR("touch init fail: %s", esp_err_to_name(err));
        return err;
    }
    err = lvgl_init();
    if (err != ESP_OK) {
        DISPLAY_ERROR("display init fail");
        return err;
    }
    set_lcd_blight(0);
    return err;
}
