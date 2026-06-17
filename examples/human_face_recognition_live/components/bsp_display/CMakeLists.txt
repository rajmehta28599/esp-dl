FILE(GLOB_RECURSE component_sources "*.c")

idf_component_register(SRCS ${component_sources}
                       INCLUDE_DIRS "include"
                       REQUIRES driver esp_lcd esp_lcd_ek79007 esp_lcd_touch
                                esp_lcd_touch_gt911 esp_lvgl_port lvgl)
