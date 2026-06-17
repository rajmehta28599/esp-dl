#ifndef _BSP_CAMERA_H_
#define _BSP_CAMERA_H_
/*
 * Camera capture engine for the CrowPanel Advanced 7" ESP32-P4 board.
 *   - Sensor: SC2336 (MIPI-CSI), configured (via sdkconfig) to output 1024x600 @ 30fps
 *   - SCCB:   I2C port 1, SCL=13, SDA=12
 *   - Pipeline: esp_video / V4L2, ISP converts RAW -> RGB565
 *
 * The engine captures frames into two PSRAM USERPTR buffers and invokes a
 * user-supplied callback for each frame. The callback runs in the capture task.
 * Ported from Elecrow's official Lesson13-Camera_Real-Time BSP (the canvas/display
 * code has been removed - rendering is handled by the UI layer).
 */
#include <string.h>
#include "esp_log.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "driver/i2c_master.h"
#include "esp_sccb_intf.h"
#include "esp_sccb_i2c.h"
#include "esp_cam_sensor.h"
#include "esp_cam_sensor_detect.h"
#include "linux/videodev2.h"
#include "esp_video_device.h"
#include "esp_video_init.h"
#include "esp_video_ioctl.h"
#include <sys/errno.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/param.h>
#include <fcntl.h>
#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_private/esp_cache_private.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CAMERA_TAG "CAMERA"
#define CAMERA_INFO(fmt, ...) ESP_LOGI(CAMERA_TAG, fmt, ##__VA_ARGS__)
#define CAMERA_DEBUG(fmt, ...) ESP_LOGD(CAMERA_TAG, fmt, ##__VA_ARGS__)
#define CAMERA_ERROR(fmt, ...) ESP_LOGE(CAMERA_TAG, fmt, ##__VA_ARGS__)

#define SCCB_MASTER_PORT 1
#define SCCB_GPIO_SCL 13
#define SCCB_GPIO_SDA 12

// Frame size the SC2336 is configured to deliver (must match the LCD/canvas).
#define CAM_H_RES 1024
#define CAM_V_RES 600
#define CAM_BYTES_PER_PIXEL 2

#define MAX_BUFFER_COUNT 10

typedef void (*camera_video_frame_operation_cb_t)(uint8_t *camera_buf,
                                                  uint8_t camera_buf_index,
                                                  uint32_t camera_buf_hes,
                                                  uint32_t camera_buf_ves,
                                                  size_t camera_buf_len);

typedef enum {
    VIDEO_TASK_DELETE = BIT(0),
    VIDEO_TASK_DELETE_DONE = BIT(1),
    VIDEO_TASK_DISPLAY_EN = BIT(2),
} video_event_id_t;

typedef struct {
    uint8_t *camera_buffer[MAX_BUFFER_COUNT];
    size_t camera_buf_size;
    uint32_t camera_buf_hes;
    uint32_t camera_buf_ves;
    struct v4l2_buffer v4l2_buf;
    uint8_t camera_mem_mode;
    camera_video_frame_operation_cb_t user_camera_video_frame_operation_cb;
    TaskHandle_t video_stream_task_handle;
    EventGroupHandle_t video_event_group;
} camera_video_t;

// Initialize SCCB bus and esp_video (CSI). Call once before camera_start().
esp_err_t camera_video_init(void);

// Register the per-frame callback. Must be called before camera_start().
esp_err_t video_register_frame_operation_cb(camera_video_frame_operation_cb_t operation_cb);

// Open the video device, allocate two PSRAM frame buffers, start the capture task.
// Returns the video fd on success, or -1 on failure.
int camera_start(int core_id);

// Enable/disable frame delivery to the callback.
void set_camera_img_display(bool state);

esp_err_t video_stream_task_stop(int video_fd);
esp_err_t video_stream_wait_stop(void);
uint32_t app_video_get_buf_size(void);

#ifdef __cplusplus
}
#endif
#endif // _BSP_CAMERA_H_
