/*
 * csi_camera_hal.c
 *
 * HAL implementation for ESP32-P4 MIPI CSI camera.
 * Pipeline: OV5647 (MIPI CSI) → ISP (RAW8→RGB565) → HW JPEG encoder → JPEG buffer
 *
 * Based on ESP-IDF examples:
 *   - examples/peripherals/camera/mipi_isp_dsi
 *   - examples/peripherals/jpeg/jpeg_encode
 */

#include "csi_camera_hal.h"

#include <string.h>
#include "esp_log.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_cache.h"
#include "esp_ldo_regulator.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "driver/i2c_master.h"
#include "driver/jpeg_encode.h"
#include "driver/isp.h"
#include "esp_cam_ctlr_csi.h"
#include "esp_cam_ctlr.h"
#include "esp_sccb_intf.h"
#include "esp_sccb_i2c.h"
#include "esp_cam_sensor.h"
#include "esp_cam_sensor_detect.h"

#define TAG "csi_camera"

// Default pin assignments for ESP32-P4 dev boards
#define DEFAULT_SCCB_SDA_IO   7
#define DEFAULT_SCCB_SCL_IO   8
#define DEFAULT_SCCB_FREQ     100000
#define DEFAULT_LDO_CHAN_ID   3
#define DEFAULT_LDO_VOLTAGE   2500  // 2.5V for MIPI CSI

#define RGB565_BYTES_PER_PIXEL 2

// Semaphore for synchronous capture
static SemaphoreHandle_t s_frame_ready_sem = NULL;
static uint8_t *s_pending_buffer = NULL;
static size_t s_pending_buflen = 0;

static bool on_get_new_trans(esp_cam_ctlr_handle_t handle, esp_cam_ctlr_trans_t *trans, void *user_data)
{
    csi_camera_t *cam = (csi_camera_t *)user_data;
    trans->buffer = cam->frame_buffer;
    trans->buflen = cam->frame_buffer_size;
    return false;
}

static bool on_trans_finished(esp_cam_ctlr_handle_t handle, esp_cam_ctlr_trans_t *trans, void *user_data)
{
    s_pending_buffer = trans->buffer;
    s_pending_buflen = trans->received_size;

    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    if (s_frame_ready_sem) {
        xSemaphoreGiveFromISR(s_frame_ready_sem, &xHigherPriorityTaskWoken);
    }
    return xHigherPriorityTaskWoken == pdTRUE;
}

static esp_err_t init_sensor(csi_camera_t *cam)
{
    int sda = cam->config.sccb_sda_io >= 0 ? cam->config.sccb_sda_io : DEFAULT_SCCB_SDA_IO;
    int scl = cam->config.sccb_scl_io >= 0 ? cam->config.sccb_scl_io : DEFAULT_SCCB_SCL_IO;

    ESP_LOGI(TAG, "init_sensor: SDA=%d, SCL=%d", sda, scl);

    // I2C bus — use I2C_NUM_1 to avoid conflict with MicroPython's I2C_NUM_0
    i2c_master_bus_config_t i2c_bus_conf = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .sda_io_num = sda,
        .scl_io_num = scl,
        .i2c_port = I2C_NUM_1,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t i2c_bus = NULL;
    esp_err_t ret = i2c_new_master_bus(&i2c_bus_conf, &i2c_bus);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2c bus init failed (port=%d): %s", I2C_NUM_1, esp_err_to_name(ret));
        return ret;
    }
    cam->i2c_bus_handle = i2c_bus;
    ESP_LOGI(TAG, "I2C bus initialized on port %d", I2C_NUM_1);

    // Auto-detect camera sensor via SCCB
    esp_cam_sensor_config_t cam_config = {
        .reset_pin = -1,
        .pwdn_pin = -1,
        .xclk_pin = -1,
    };

    esp_cam_sensor_device_t *sensor = NULL;
    int detect_count = 0;
    for (esp_cam_sensor_detect_fn_t *p = &__esp_cam_sensor_detect_fn_array_start;
         p < &__esp_cam_sensor_detect_fn_array_end; ++p) {
        detect_count++;
        ESP_LOGI(TAG, "trying sensor detect[%d]: addr=0x%02x, port=%d", detect_count, p->sccb_addr, p->port);
        sccb_i2c_config_t i2c_config = {
            .scl_speed_hz = DEFAULT_SCCB_FREQ,
            .device_address = p->sccb_addr,
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        };
        ret = sccb_new_i2c_io(i2c_bus, &i2c_config, &cam_config.sccb_handle);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "sccb io init failed for addr 0x%02x: %s", p->sccb_addr, esp_err_to_name(ret));
            return ret;
        }

        cam_config.sensor_port = p->port;
        sensor = (*(p->detect))(&cam_config);
        if (sensor) {
            if (p->port != ESP_CAM_SENSOR_MIPI_CSI) {
                ESP_LOGE(TAG, "detected sensor with non-CSI interface (port=%d), skipping", p->port);
                sensor = NULL;
                esp_sccb_del_i2c_io(cam_config.sccb_handle);
                continue;
            }
            ESP_LOGI(TAG, "sensor detected: addr=0x%02x", p->sccb_addr);
            break;
        }
        esp_sccb_del_i2c_io(cam_config.sccb_handle);
    }

    ESP_LOGI(TAG, "scanned %d sensor driver(s), found=%s", detect_count, sensor ? "yes" : "no");

    if (!sensor) {
        ESP_LOGE(TAG, "no camera sensor detected");
        return ESP_ERR_NOT_FOUND;
    }

    cam->sccb_handle = cam_config.sccb_handle;

    // Find a suitable format matching our resolution
    esp_cam_sensor_format_array_t fmt_array = {0};
    esp_cam_sensor_query_format(sensor, &fmt_array);

    // Try to find a RAW8 format matching resolution, or use first available
    esp_cam_sensor_format_t *best_fmt = NULL;
    char target_fmt[64];
    snprintf(target_fmt, sizeof(target_fmt), "MIPI_2lane_24Minput_RAW8_%dx%d",
             cam->config.h_res, cam->config.v_res);

    for (int i = 0; i < fmt_array.count; i++) {
        ESP_LOGI(TAG, "sensor format[%d]: %s", i, fmt_array.format_array[i].name);
        if (strstr(fmt_array.format_array[i].name, target_fmt)) {
            best_fmt = (esp_cam_sensor_format_t *)&fmt_array.format_array[i];
            break;
        }
    }

    if (!best_fmt && fmt_array.count > 0) {
        best_fmt = (esp_cam_sensor_format_t *)&fmt_array.format_array[0];
        ESP_LOGW(TAG, "exact format not found, using: %s", best_fmt->name);
    }

    if (!best_fmt) {
        ESP_LOGE(TAG, "no sensor format available");
        return ESP_ERR_NOT_SUPPORTED;
    }

    ret = esp_cam_sensor_set_format(sensor, best_fmt);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "set format failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // Update config to match actual sensor output resolution
    if (cam->config.h_res != best_fmt->width || cam->config.v_res != best_fmt->height) {
        ESP_LOGE(TAG, "resolution adjusted: %dx%d -> %dx%d (sensor format: %s)",
                 cam->config.h_res, cam->config.v_res,
                 best_fmt->width, best_fmt->height, best_fmt->name);
        cam->config.h_res = best_fmt->width;
        cam->config.v_res = best_fmt->height;
    }
    ESP_LOGE(TAG, "sensor format set: %s (%dx%d)", best_fmt->name, best_fmt->width, best_fmt->height);

    // NOTE: Do NOT start stream here — CSI/ISP must be ready first.
    // Stream will be started after CSI and ISP are initialized.
    cam->sensor_handle = sensor;

    return ESP_OK;
}

esp_err_t csi_camera_init(csi_camera_t *cam)
{
    esp_err_t ret;

    if (cam->initialized) {
        // Auto-deinit before re-init
        csi_camera_deinit(cam);
    }

    cam->init_step = 0;

    // Create semaphore for frame synchronization
    if (!s_frame_ready_sem) {
        s_frame_ready_sem = xSemaphoreCreateBinary();
    }

    // Step 1: LDO for MIPI CSI PHY power (2.5V)
    esp_ldo_channel_config_t ldo_cfg = {
        .chan_id = DEFAULT_LDO_CHAN_ID,
        .voltage_mv = DEFAULT_LDO_VOLTAGE,
    };
    esp_ldo_channel_handle_t ldo_handle = NULL;
    ret = esp_ldo_acquire_channel(&ldo_cfg, &ldo_handle);
    if (ret != ESP_OK) { cam->init_step = 1; return ret; }
    cam->ldo_handle = ldo_handle;

    // Step 2: Initialize camera sensor (I2C + SCCB + auto-detect)
    ret = init_sensor(cam);
    if (ret != ESP_OK) { cam->init_step = 2; return ret; }

    // Step 3: Allocate frame buffer in PSRAM for RGB565 data
    cam->frame_buffer_size = cam->config.h_res * cam->config.v_res * RGB565_BYTES_PER_PIXEL;
    cam->frame_buffer = (uint8_t *)heap_caps_aligned_calloc(64, 1, cam->frame_buffer_size,
                                                             MALLOC_CAP_SPIRAM);
    if (!cam->frame_buffer) {
        cam->init_step = 3;
        return ESP_ERR_NO_MEM;
    }

    // Step 4: CSI controller
    esp_cam_ctlr_csi_config_t csi_config = {
        .ctlr_id = 0,
        .h_res = cam->config.h_res,
        .v_res = cam->config.v_res,
        .lane_bit_rate_mbps = cam->config.lane_bitrate_mbps,
        .input_data_color_type = CAM_CTLR_COLOR_RAW8,
        .output_data_color_type = CAM_CTLR_COLOR_RGB565,
        .data_lane_num = cam->config.data_lanes,
        .byte_swap_en = false,
        .queue_items = 1,
    };
    esp_cam_ctlr_handle_t cam_handle = NULL;
    ret = esp_cam_new_csi_ctlr(&csi_config, &cam_handle);
    if (ret != ESP_OK) { cam->init_step = 4; return ret; }
    cam->cam_handle = cam_handle;

    esp_cam_ctlr_evt_cbs_t cbs = {
        .on_get_new_trans = NULL,  // No auto-chain — we use explicit receive() per capture
        .on_trans_finished = on_trans_finished,
    };
    ret = esp_cam_ctlr_register_event_callbacks(cam_handle, &cbs, cam);
    if (ret != ESP_OK) { cam->init_step = 41; return ret; }
    ret = esp_cam_ctlr_enable(cam_handle);
    if (ret != ESP_OK) { cam->init_step = 42; return ret; }

    // Step 5: ISP processor (RAW8 → RGB565)
    isp_proc_handle_t isp_handle = NULL;
    esp_isp_processor_cfg_t isp_config = {
        .clk_hz = 80 * 1000 * 1000,
        .input_data_source = ISP_INPUT_DATA_SOURCE_CSI,
        .input_data_color_type = ISP_COLOR_RAW8,
        .output_data_color_type = ISP_COLOR_RGB565,
        .has_line_start_packet = true,
        .has_line_end_packet = true,
        .h_res = cam->config.h_res,
        .v_res = cam->config.v_res,
    };
    ret = esp_isp_new_processor(&isp_config, &isp_handle);
    if (ret != ESP_OK) { cam->init_step = 5; return ret; }
    ret = esp_isp_enable(isp_handle);
    if (ret != ESP_OK) { cam->init_step = 51; return ret; }
    cam->isp_handle = isp_handle;

    // Step 6: Hardware JPEG encoder
    jpeg_encoder_handle_t jpeg_handle = NULL;
    jpeg_encode_engine_cfg_t enc_eng_cfg = {
        .timeout_ms = 200,
    };
    ret = jpeg_new_encoder_engine(&enc_eng_cfg, &jpeg_handle);
    if (ret != ESP_OK) { cam->init_step = 6; return ret; }
    cam->jpeg_handle = jpeg_handle;

    // Allocate JPEG output buffer
    jpeg_encode_memory_alloc_cfg_t rx_mem_cfg = {
        .buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER,
    };
    cam->jpeg_buffer = (uint8_t *)jpeg_alloc_encoder_mem(
        cam->frame_buffer_size / 3, &rx_mem_cfg, &cam->jpeg_buffer_size);
    if (!cam->jpeg_buffer) {
        cam->init_step = 61;
        return ESP_ERR_NO_MEM;
    }

    // Step 7: Start sensor stream (MUST be after CSI and ISP are ready)
    int enable = 1;
    ret = esp_cam_sensor_ioctl(cam->sensor_handle, ESP_CAM_SENSOR_IOC_S_STREAM, &enable);
    if (ret != ESP_OK) { cam->init_step = 7; return ret; }

    // Step 8: Start CSI capture
    ret = esp_cam_ctlr_start(cam_handle);
    if (ret != ESP_OK) { cam->init_step = 8; return ret; }

    cam->initialized = true;
    cam->frame_captured = false;
    ESP_LOGI(TAG, "CSI camera initialized: %dx%d, JPEG quality=%d",
             cam->config.h_res, cam->config.v_res, cam->config.jpeg_quality);

    return ESP_OK;
}

esp_err_t csi_camera_capture(csi_camera_t *cam)
{
    if (!cam->initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    // Clear any stale semaphore signal from previous capture
    xSemaphoreTake(s_frame_ready_sem, 0);

    // Request a new frame from CSI
    esp_cam_ctlr_trans_t trans = {
        .buffer = cam->frame_buffer,
        .buflen = cam->frame_buffer_size,
    };
    esp_err_t ret = esp_cam_ctlr_receive(cam->cam_handle, &trans, 2000);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "CSI receive failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // Wait for frame completion
    if (xSemaphoreTake(s_frame_ready_sem, pdMS_TO_TICKS(5000)) != pdTRUE) {
        ESP_LOGE(TAG, "frame capture timeout");
        return ESP_ERR_TIMEOUT;
    }

    // Invalidate cache to ensure JPEG encoder sees fresh DMA-written data
    esp_cache_msync(cam->frame_buffer, cam->frame_buffer_size, ESP_CACHE_MSYNC_FLAG_DIR_M2C);

    // Encode RGB565 frame to JPEG using hardware encoder
    jpeg_encode_cfg_t enc_cfg = {
        .src_type = JPEG_ENCODE_IN_FORMAT_RGB565,
        .sub_sample = JPEG_DOWN_SAMPLING_YUV422,
        .image_quality = cam->config.jpeg_quality,
        .width = cam->config.h_res,
        .height = cam->config.v_res,
    };

    uint32_t jpeg_size = 0;
    ret = jpeg_encoder_process(cam->jpeg_handle, &enc_cfg,
                               cam->frame_buffer, cam->frame_buffer_size,
                               cam->jpeg_buffer, cam->jpeg_buffer_size,
                               &jpeg_size);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "JPEG encode failed: %s", esp_err_to_name(ret));
        return ret;
    }

    cam->jpeg_data_size = jpeg_size;
    cam->frame_captured = true;

    return ESP_OK;
}

void csi_camera_free_buffer(csi_camera_t *cam)
{
    cam->frame_captured = false;
    cam->jpeg_data_size = 0;
}

esp_err_t csi_camera_deinit(csi_camera_t *cam)
{
    if (!cam->initialized) {
        return ESP_OK;
    }

    cam->initialized = false;

    // Stop sensor stream first
    if (cam->sensor_handle) {
        int disable = 0;
        esp_cam_sensor_ioctl(cam->sensor_handle, ESP_CAM_SENSOR_IOC_S_STREAM, &disable);
        esp_cam_sensor_del_dev(cam->sensor_handle);
        cam->sensor_handle = NULL;
    }

    if (cam->cam_handle) {
        esp_cam_ctlr_stop(cam->cam_handle);
        esp_cam_ctlr_disable(cam->cam_handle);
        esp_cam_ctlr_del(cam->cam_handle);
        cam->cam_handle = NULL;
    }

    if (cam->isp_handle) {
        esp_isp_disable(cam->isp_handle);
        esp_isp_del_processor(cam->isp_handle);
        cam->isp_handle = NULL;
    }

    if (cam->jpeg_handle) {
        jpeg_del_encoder_engine(cam->jpeg_handle);
        cam->jpeg_handle = NULL;
    }

    if (cam->sccb_handle) {
        esp_sccb_del_i2c_io(cam->sccb_handle);
        cam->sccb_handle = NULL;
    }

    if (cam->i2c_bus_handle) {
        i2c_del_master_bus(cam->i2c_bus_handle);
        cam->i2c_bus_handle = NULL;
    }

    if (cam->ldo_handle) {
        esp_ldo_release_channel(cam->ldo_handle);
        cam->ldo_handle = NULL;
    }

    if (cam->frame_buffer) {
        heap_caps_free(cam->frame_buffer);
        cam->frame_buffer = NULL;
    }

    if (cam->jpeg_buffer) {
        free(cam->jpeg_buffer);
        cam->jpeg_buffer = NULL;
    }

    if (s_frame_ready_sem) {
        vSemaphoreDelete(s_frame_ready_sem);
        s_frame_ready_sem = NULL;
    }

    ESP_LOGI(TAG, "CSI camera deinitialized");
    return ESP_OK;
}
