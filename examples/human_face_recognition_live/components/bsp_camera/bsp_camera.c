#include "bsp_camera.h"
#include <inttypes.h>

static i2c_master_bus_handle_t sccb_bus_handle = NULL;
static camera_video_t camera_video;
static uint8_t *cam_buffer[2];
static int camera_video_id = 0;

esp_err_t camera_video_init(void)
{
    esp_err_t err = ESP_OK;
    i2c_master_bus_config_t sccb_conf = {
        .i2c_port = SCCB_MASTER_PORT,
        .sda_io_num = SCCB_GPIO_SDA,
        .scl_io_num = SCCB_GPIO_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .flags.enable_internal_pullup = true,
    };
    CAMERA_INFO("Initializing SCCB Bus.......");
    err = i2c_new_master_bus(&sccb_conf, &sccb_bus_handle);
    if (err != ESP_OK)
        return err;

    esp_video_init_csi_config_t csi_config = {
        .sccb_config = {
            .init_sccb = false,
            .i2c_handle = sccb_bus_handle,
            .freq = 100000,
        },
        .reset_pin = -1,
        .pwdn_pin = -1,
    };
    esp_video_init_config_t cam_config = {
        .csi = &csi_config,
    };
    return esp_video_init(&cam_config);
}

static int video_open(void)
{
    struct v4l2_format camera_format;
    struct v4l2_capability capability;

    int fd = open(ESP_VIDEO_MIPI_CSI_DEVICE_NAME, O_RDONLY | O_NONBLOCK, 0);
    if (fd < 0) {
        CAMERA_ERROR("Open video failed");
        return -1;
    }
    if (ioctl(fd, VIDIOC_QUERYCAP, &capability)) {
        CAMERA_ERROR("failed to get capability");
        goto exit_0;
    }
    CAMERA_INFO("driver:  %s", capability.driver);
    CAMERA_INFO("card:    %s", capability.card);

    memset(&camera_format, 0, sizeof(struct v4l2_format));
    camera_format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(fd, VIDIOC_G_FMT, &camera_format) != 0) {
        CAMERA_ERROR("failed to get format");
        goto exit_0;
    }
    CAMERA_INFO("camera native: width=%" PRIu32 " height=%" PRIu32,
                camera_format.fmt.pix.width, camera_format.fmt.pix.height);
    camera_video.camera_buf_hes = camera_format.fmt.pix.width;
    camera_video.camera_buf_ves = camera_format.fmt.pix.height;

    if (camera_format.fmt.pix.pixelformat != V4L2_PIX_FMT_RGB565) {
        struct v4l2_format format = {
            .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
            .fmt.pix.width = camera_format.fmt.pix.width,
            .fmt.pix.height = camera_format.fmt.pix.height,
            .fmt.pix.pixelformat = V4L2_PIX_FMT_RGB565,
        };
        if (ioctl(fd, VIDIOC_S_FMT, &format) != 0) {
            CAMERA_ERROR("failed to set RGB565 format");
            goto exit_0;
        }
    }

    // Safety check: the sensor must deliver frames that fit our fixed buffers.
    if (camera_video.camera_buf_hes != CAM_H_RES || camera_video.camera_buf_ves != CAM_V_RES) {
        CAMERA_ERROR("camera resolution %ux%u != expected %ux%u; check sdkconfig SC2336 format",
                     (unsigned)camera_video.camera_buf_hes, (unsigned)camera_video.camera_buf_ves,
                     CAM_H_RES, CAM_V_RES);
        goto exit_0;
    }

    CAMERA_INFO("video opened, fd = %d", fd);
    return fd;
exit_0:
    close(fd);
    return -1;
}

static esp_err_t camera_video_set_bufs(int video_fd, uint32_t fb_num, const void **fb)
{
    struct v4l2_requestbuffers req;
    if (fb_num > MAX_BUFFER_COUNT || fb_num < 2) {
        CAMERA_ERROR("invalid buffer count");
        return ESP_FAIL;
    }
    memset(&req, 0, sizeof(req));
    req.count = fb_num;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    camera_video.camera_mem_mode = req.memory = fb ? V4L2_MEMORY_USERPTR : V4L2_MEMORY_MMAP;
    if (ioctl(video_fd, VIDIOC_REQBUFS, &req) != 0) {
        CAMERA_ERROR("req bufs failed");
        goto errout;
    }
    for (uint32_t i = 0; i < fb_num; i++) {
        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = req.memory;
        buf.index = i;
        if (ioctl(video_fd, VIDIOC_QUERYBUF, &buf) != 0) {
            CAMERA_ERROR("query buf failed");
            goto errout;
        }
        if (req.memory == V4L2_MEMORY_MMAP) {
            camera_video.camera_buffer[i] =
                mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, video_fd, buf.m.offset);
            if (camera_video.camera_buffer[i] == NULL) {
                CAMERA_ERROR("mmap failed");
                goto errout;
            }
        } else {
            if (!fb[i]) {
                CAMERA_ERROR("frame buffer is NULL");
                goto errout;
            }
            buf.m.userptr = (unsigned long)fb[i];
            camera_video.camera_buffer[i] = (uint8_t *)fb[i];
        }
        camera_video.camera_buf_size = buf.length;
        if (ioctl(video_fd, VIDIOC_QBUF, &buf) != 0) {
            CAMERA_ERROR("queue frame buffer failed");
            goto errout;
        }
    }
    return ESP_OK;
errout:
    close(video_fd);
    return ESP_FAIL;
}

uint32_t app_video_get_buf_size(void)
{
    return camera_video.camera_buf_hes * camera_video.camera_buf_ves * CAM_BYTES_PER_PIXEL;
}

static inline esp_err_t video_receive_video_frame(int video_fd)
{
    memset(&camera_video.v4l2_buf, 0, sizeof(camera_video.v4l2_buf));
    camera_video.v4l2_buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    camera_video.v4l2_buf.memory = camera_video.camera_mem_mode;
    if (ioctl(video_fd, VIDIOC_DQBUF, &(camera_video.v4l2_buf)) != 0) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

static inline void video_operation_video_frame(void)
{
    uint8_t buf_index = camera_video.v4l2_buf.index;
    camera_video.v4l2_buf.m.userptr = (unsigned long)camera_video.camera_buffer[buf_index];
    camera_video.v4l2_buf.length = camera_video.camera_buf_size;
    if (camera_video.user_camera_video_frame_operation_cb) {
        camera_video.user_camera_video_frame_operation_cb(
            camera_video.camera_buffer[buf_index], buf_index,
            camera_video.camera_buf_hes, camera_video.camera_buf_ves,
            camera_video.camera_buf_size);
    }
}

static inline esp_err_t video_free_video_frame(int video_fd)
{
    if (ioctl(video_fd, VIDIOC_QBUF, &(camera_video.v4l2_buf)) != 0) {
        CAMERA_ERROR("failed to free video frame");
        return ESP_FAIL;
    }
    return ESP_OK;
}

static inline esp_err_t video_stream_start(int video_fd)
{
    CAMERA_INFO("Video Stream Start");
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(video_fd, VIDIOC_STREAMON, &type)) {
        CAMERA_ERROR("failed to start stream");
        return ESP_FAIL;
    }
    return ESP_OK;
}

static inline esp_err_t video_stream_stop(int video_fd)
{
    CAMERA_INFO("Video Stream Stop");
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(video_fd, VIDIOC_STREAMOFF, &type)) {
        CAMERA_ERROR("failed to stop stream");
        return ESP_FAIL;
    }
    xEventGroupSetBits(camera_video.video_event_group, VIDEO_TASK_DELETE_DONE);
    return ESP_OK;
}

static void video_stream_task(void *arg)
{
    int video_fd = *((int *)arg);
    while (1) {
        EventBits_t bits = xEventGroupGetBits(camera_video.video_event_group);
        if (bits & VIDEO_TASK_DELETE) {
            xEventGroupClearBits(camera_video.video_event_group, VIDEO_TASK_DELETE);
            video_stream_stop(video_fd);
            break;
        }
        if (bits & VIDEO_TASK_DISPLAY_EN) {
            if (video_receive_video_frame(video_fd) != ESP_OK) {
                continue;
            }
            video_operation_video_frame();
            video_free_video_frame(video_fd);
        } else {
            vTaskDelay(pdMS_TO_TICKS(33));
        }
    }
    vTaskDelete(NULL);
}

esp_err_t video_register_frame_operation_cb(camera_video_frame_operation_cb_t operation_cb)
{
    camera_video.user_camera_video_frame_operation_cb = operation_cb;
    return ESP_OK;
}

static esp_err_t video_stream_task_start(int video_fd, int core_id)
{
    if (camera_video.video_event_group == NULL) {
        camera_video.video_event_group = xEventGroupCreate();
    }
    xEventGroupClearBits(camera_video.video_event_group, VIDEO_TASK_DELETE_DONE | VIDEO_TASK_DISPLAY_EN);
    if (video_stream_start(video_fd) != ESP_OK) {
        return ESP_FAIL;
    }
    static int s_video_fd;
    s_video_fd = video_fd;
    // 16KB stack: the frame callback runs LVGL rendering (lv_refr_now composites the
    // status/button glyphs over the canvas) on this task, which the stock 4KB cannot hold.
    BaseType_t result = xTaskCreatePinnedToCore(video_stream_task, "video_stream", 16384, &s_video_fd, 3,
                                                &camera_video.video_stream_task_handle, core_id);
    if (result != pdPASS) {
        CAMERA_ERROR("failed to create video stream task");
        video_stream_stop(video_fd);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t video_stream_task_stop(int video_fd)
{
    xEventGroupSetBits(camera_video.video_event_group, VIDEO_TASK_DELETE);
    return ESP_OK;
}

esp_err_t video_stream_wait_stop(void)
{
    xEventGroupWaitBits(camera_video.video_event_group, VIDEO_TASK_DELETE_DONE, pdTRUE, pdTRUE, portMAX_DELAY);
    return ESP_OK;
}

int camera_start(int core_id)
{
    size_t cache_line_size = 0;
    camera_video_id = video_open();
    if (camera_video_id < 0) {
        return -1;
    }
    esp_cache_get_alignment(MALLOC_CAP_SPIRAM, &cache_line_size);
    for (int i = 0; i < 2; i++) {
        cam_buffer[i] = (uint8_t *)heap_caps_aligned_alloc(
            cache_line_size, CAM_H_RES * CAM_V_RES * CAM_BYTES_PER_PIXEL, MALLOC_CAP_SPIRAM);
        if (!cam_buffer[i]) {
            CAMERA_ERROR("failed to allocate camera buffer %d", i);
            return -1;
        }
        memset(cam_buffer[i], 0, CAM_H_RES * CAM_V_RES * CAM_BYTES_PER_PIXEL);
    }
    if (camera_video_set_bufs(camera_video_id, 2, (const void **)cam_buffer) != ESP_OK) {
        return -1;
    }
    if (video_stream_task_start(camera_video_id, core_id) != ESP_OK) {
        return -1;
    }
    return camera_video_id;
}

void set_camera_img_display(bool state)
{
    if (!camera_video.video_event_group) {
        return;
    }
    if (state) {
        xEventGroupSetBits(camera_video.video_event_group, VIDEO_TASK_DISPLAY_EN);
    } else {
        xEventGroupClearBits(camera_video.video_event_group, VIDEO_TASK_DISPLAY_EN);
    }
}
