/*
 * csi_camera_hal.c
 *
 * HAL implementation for ESP32-P4 MIPI CSI camera.
 * Pipeline: OV5647 (MIPI CSI) → ISP (RAW8→RGB565) → HW JPEG encoder → JPEG buffer
 *
 * Uses singleton pattern: hardware state persists in a static instance so that
 * re-running a Python script without explicit deinit() reuses the existing
 * camera pipeline instead of failing with ESP_ERR_INVALID_STATE.
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
#include "driver/isp_color.h"
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

// ---- Singleton instance ----
static csi_camera_t s_cam_instance = {0};

// Semaphore for synchronous capture
static SemaphoreHandle_t s_frame_ready_sem = NULL;
static uint8_t *s_pending_buffer = NULL;
static size_t s_pending_buflen = 0;

csi_camera_t *csi_camera_get_instance(void)
{
    return &s_cam_instance;
}

// Update crop state when target resolution changes
static esp_err_t update_crop(csi_camera_t *cam, uint16_t h_res, uint16_t v_res)
{
    bool needs = (h_res != cam->config.sensor_h_res || v_res != cam->config.sensor_v_res);

    if (needs && (h_res > cam->config.sensor_h_res || v_res > cam->config.sensor_v_res)) {
        ESP_LOGE(TAG, "target %dx%d exceeds sensor %dx%d",
                 h_res, v_res, cam->config.sensor_h_res, cam->config.sensor_v_res);
        h_res = cam->config.sensor_h_res;
        v_res = cam->config.sensor_v_res;
        needs = false;
    }

    cam->config.h_res = h_res;
    cam->config.v_res = v_res;
    cam->needs_crop = needs;

    if (needs) {
        size_t needed = h_res * v_res * RGB565_BYTES_PER_PIXEL;
        if (!cam->crop_buffer || cam->crop_buffer_size < needed) {
            if (cam->crop_buffer) {
                heap_caps_free(cam->crop_buffer);
            }
            cam->crop_buffer = (uint8_t *)heap_caps_aligned_calloc(
                64, 1, needed, MALLOC_CAP_SPIRAM);
            if (!cam->crop_buffer) {
                cam->crop_buffer_size = 0;
                return ESP_ERR_NO_MEM;
            }
            cam->crop_buffer_size = needed;
        }
    }
    return ESP_OK;
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
        sccb_i2c_config_t i2c_config = {
            .scl_speed_hz = DEFAULT_SCCB_FREQ,
            .device_address = p->sccb_addr,
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        };
        ret = sccb_new_i2c_io(i2c_bus, &i2c_config, &cam_config.sccb_handle);
        if (ret != ESP_OK) continue;

        cam_config.sensor_port = p->port;
        sensor = (*(p->detect))(&cam_config);
        if (sensor) {
            if (p->port != ESP_CAM_SENSOR_MIPI_CSI) {
                sensor = NULL;
                esp_sccb_del_i2c_io(cam_config.sccb_handle);
                continue;
            }
            ESP_LOGE(TAG, "sensor detected: addr=0x%02x", p->sccb_addr);
            break;
        }
        esp_sccb_del_i2c_io(cam_config.sccb_handle);
    }

    if (!sensor) {
        ESP_LOGE(TAG, "no camera sensor detected");
        return ESP_ERR_NOT_FOUND;
    }

    cam->sccb_handle = cam_config.sccb_handle;

    // Find best sensor format
    esp_cam_sensor_format_array_t fmt_array = {0};
    esp_cam_sensor_query_format(sensor, &fmt_array);

    esp_cam_sensor_format_t *best_fmt = NULL;
    esp_cam_sensor_format_t *smallest_covering = NULL;
    uint32_t smallest_area = UINT32_MAX;
    char target_fmt[64];
    snprintf(target_fmt, sizeof(target_fmt), "MIPI_2lane_24Minput_RAW8_%dx%d",
             cam->config.h_res, cam->config.v_res);

    for (int i = 0; i < fmt_array.count; i++) {
        ESP_LOGE(TAG, "sensor format[%d]: %s (%dx%d)", i,
                 fmt_array.format_array[i].name,
                 fmt_array.format_array[i].width,
                 fmt_array.format_array[i].height);
        if (strstr(fmt_array.format_array[i].name, target_fmt)) {
            best_fmt = (esp_cam_sensor_format_t *)&fmt_array.format_array[i];
            break;
        }
        if (strstr(fmt_array.format_array[i].name, "RAW8") &&
            fmt_array.format_array[i].width >= cam->config.h_res &&
            fmt_array.format_array[i].height >= cam->config.v_res) {
            uint32_t area = fmt_array.format_array[i].width * fmt_array.format_array[i].height;
            if (area < smallest_area) {
                smallest_area = area;
                smallest_covering = (esp_cam_sensor_format_t *)&fmt_array.format_array[i];
            }
        }
    }

    if (!best_fmt && smallest_covering) {
        best_fmt = smallest_covering;
    }
    if (!best_fmt && fmt_array.count > 0) {
        best_fmt = (esp_cam_sensor_format_t *)&fmt_array.format_array[0];
    }
    if (!best_fmt) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    ret = esp_cam_sensor_set_format(sensor, best_fmt);
    if (ret != ESP_OK) return ret;

    cam->config.sensor_h_res = best_fmt->width;
    cam->config.sensor_v_res = best_fmt->height;
    cam->sensor_handle = sensor;

    ESP_LOGE(TAG, "sensor format: %s (%dx%d)", best_fmt->name, best_fmt->width, best_fmt->height);
    return ESP_OK;
}

esp_err_t csi_camera_init(csi_camera_t *cam)
{
    // ---- Singleton reuse logic ----
    csi_camera_t *inst = &s_cam_instance;

    if (inst->initialized) {
        // Hardware is already up — check if config is compatible
        // Save requested target resolution before comparing
        csi_camera_config_t new_cfg = cam->config;

        // For comparison, we need to know what sensor resolution the new config would use.
        // If the hardware pipeline (sensor, CSI, ISP) is the same, we can reuse it.
        // The sensor resolution is determined during init_sensor, but we can check if
        // the existing sensor covers the new target.
        bool can_reuse = (inst->config.sensor_h_res >= new_cfg.h_res &&
                          inst->config.sensor_v_res >= new_cfg.v_res &&
                          inst->config.data_lanes == new_cfg.data_lanes &&
                          inst->config.lane_bitrate_mbps == new_cfg.lane_bitrate_mbps);

        if (can_reuse) {
            ESP_LOGE(TAG, "reusing existing camera pipeline (sensor=%dx%d)",
                     inst->config.sensor_h_res, inst->config.sensor_v_res);

            // Update target resolution & crop buffer
            esp_err_t ret = update_crop(inst, new_cfg.h_res, new_cfg.v_res);
            if (ret != ESP_OK) return ret;

            // Update jpeg quality (soft parameter)
            inst->config.jpeg_quality = new_cfg.jpeg_quality;

            // Copy singleton state back to caller
            memcpy(cam, inst, sizeof(csi_camera_t));
            return ESP_OK;
        }

        // Incompatible config — full deinit first
        ESP_LOGE(TAG, "config changed, reinitializing camera");
        csi_camera_deinit(inst);
    }

    // ---- Fresh initialization ----
    // Copy user config into singleton
    memcpy(&inst->config, &cam->config, sizeof(csi_camera_config_t));
    inst->init_step = 0;
    inst->color = (csi_camera_color_t){
        .brightness = 0,
        .contrast = 128,
        .saturation = 128,
        .hue = 0,
        .enabled = false,
    };

    esp_err_t ret;

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
    if (ret != ESP_OK) { inst->init_step = 1; goto fail; }
    inst->ldo_handle = ldo_handle;

    // Step 2: Initialize camera sensor (I2C + SCCB + auto-detect)
    ret = init_sensor(inst);
    if (ret != ESP_OK) { inst->init_step = 2; goto fail; }

    // Compute crop state
    ret = update_crop(inst, inst->config.h_res, inst->config.v_res);
    if (ret != ESP_OK) { inst->init_step = 31; goto fail; }

    // Step 3: Allocate frame buffer (sensor resolution)
    uint16_t s_h = inst->config.sensor_h_res;
    uint16_t s_v = inst->config.sensor_v_res;
    inst->frame_buffer_size = s_h * s_v * RGB565_BYTES_PER_PIXEL;
    inst->frame_buffer = (uint8_t *)heap_caps_aligned_calloc(
        64, 1, inst->frame_buffer_size, MALLOC_CAP_SPIRAM);
    if (!inst->frame_buffer) {
        inst->init_step = 3;
        ret = ESP_ERR_NO_MEM;
        goto fail;
    }

    // Step 4: CSI controller
    esp_cam_ctlr_csi_config_t csi_config = {
        .ctlr_id = 0,
        .h_res = s_h,
        .v_res = s_v,
        .lane_bit_rate_mbps = inst->config.lane_bitrate_mbps,
        .input_data_color_type = CAM_CTLR_COLOR_RAW8,
        .output_data_color_type = CAM_CTLR_COLOR_RGB565,
        .data_lane_num = inst->config.data_lanes,
        .byte_swap_en = false,
        .queue_items = 1,
    };
    esp_cam_ctlr_handle_t cam_handle = NULL;
    ret = esp_cam_new_csi_ctlr(&csi_config, &cam_handle);
    if (ret != ESP_OK) { inst->init_step = 4; goto fail; }
    inst->cam_handle = cam_handle;

    esp_cam_ctlr_evt_cbs_t cbs = {
        .on_get_new_trans = NULL,
        .on_trans_finished = on_trans_finished,
    };
    ret = esp_cam_ctlr_register_event_callbacks(cam_handle, &cbs, inst);
    if (ret != ESP_OK) { inst->init_step = 41; goto fail; }
    ret = esp_cam_ctlr_enable(cam_handle);
    if (ret != ESP_OK) { inst->init_step = 42; goto fail; }

    // Step 5: ISP processor (RAW8 → RGB565)
    isp_proc_handle_t isp_handle = NULL;
    esp_isp_processor_cfg_t isp_config = {
        .clk_hz = 80 * 1000 * 1000,
        .input_data_source = ISP_INPUT_DATA_SOURCE_CSI,
        .input_data_color_type = ISP_COLOR_RAW8,
        .output_data_color_type = ISP_COLOR_RGB565,
        .has_line_start_packet = true,
        .has_line_end_packet = true,
        .h_res = s_h,
        .v_res = s_v,
    };
    ret = esp_isp_new_processor(&isp_config, &isp_handle);
    if (ret != ESP_OK) { inst->init_step = 5; goto fail; }
    ret = esp_isp_enable(isp_handle);
    if (ret != ESP_OK) { inst->init_step = 51; goto fail; }
    inst->isp_handle = isp_handle;

    // Step 6: Hardware JPEG encoder
    jpeg_encoder_handle_t jpeg_handle = NULL;
    jpeg_encode_engine_cfg_t enc_eng_cfg = {
        .timeout_ms = 200,
    };
    ret = jpeg_new_encoder_engine(&enc_eng_cfg, &jpeg_handle);
    if (ret != ESP_OK) { inst->init_step = 6; goto fail; }
    inst->jpeg_handle = jpeg_handle;

    jpeg_encode_memory_alloc_cfg_t rx_mem_cfg = {
        .buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER,
    };
    inst->jpeg_buffer = (uint8_t *)jpeg_alloc_encoder_mem(
        inst->frame_buffer_size / 3, &rx_mem_cfg, &inst->jpeg_buffer_size);
    if (!inst->jpeg_buffer) {
        inst->init_step = 61;
        ret = ESP_ERR_NO_MEM;
        goto fail;
    }

    // Step 7: Start sensor stream
    int enable = 1;
    ret = esp_cam_sensor_ioctl(inst->sensor_handle, ESP_CAM_SENSOR_IOC_S_STREAM, &enable);
    if (ret != ESP_OK) { inst->init_step = 7; goto fail; }

    // Step 8: Start CSI capture
    ret = esp_cam_ctlr_start(cam_handle);
    if (ret != ESP_OK) { inst->init_step = 8; goto fail; }

    inst->initialized = true;
    inst->frame_captured = false;

    if (inst->needs_crop) {
        ESP_LOGE(TAG, "camera init OK: sensor=%dx%d, output=%dx%d, quality=%d",
                 s_h, s_v, inst->config.h_res, inst->config.v_res, inst->config.jpeg_quality);
    } else {
        ESP_LOGE(TAG, "camera init OK: %dx%d, quality=%d",
                 inst->config.h_res, inst->config.v_res, inst->config.jpeg_quality);
    }

    // Copy singleton to caller
    memcpy(cam, inst, sizeof(csi_camera_t));
    return ESP_OK;

fail:
    // On failure, clean up partially initialized singleton
    cam->init_step = inst->init_step;
    csi_camera_deinit(inst);
    return ret;
}

esp_err_t csi_camera_capture(csi_camera_t *cam)
{
    // Always operate on the singleton
    csi_camera_t *inst = &s_cam_instance;
    if (!inst->initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    // Clear any stale semaphore signal
    xSemaphoreTake(s_frame_ready_sem, 0);

    // Request a new frame from CSI
    esp_cam_ctlr_trans_t trans = {
        .buffer = inst->frame_buffer,
        .buflen = inst->frame_buffer_size,
    };
    esp_err_t ret = esp_cam_ctlr_receive(inst->cam_handle, &trans, 2000);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "CSI receive failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // Wait for frame completion
    if (xSemaphoreTake(s_frame_ready_sem, pdMS_TO_TICKS(5000)) != pdTRUE) {
        ESP_LOGE(TAG, "frame capture timeout");
        return ESP_ERR_TIMEOUT;
    }

    // Invalidate cache to see DMA-written data
    esp_cache_msync(inst->frame_buffer, inst->frame_buffer_size, ESP_CACHE_MSYNC_FLAG_DIR_M2C);

    // Center crop if needed
    uint8_t *encode_buf;
    size_t encode_buf_size;
    uint16_t encode_w, encode_h;

    if (inst->needs_crop && inst->crop_buffer) {
        uint16_t s_w = inst->config.sensor_h_res;
        uint16_t t_w = inst->config.h_res;
        uint16_t t_h = inst->config.v_res;
        uint16_t off_x = (s_w - t_w) / 2;
        uint16_t off_y = (inst->config.sensor_v_res - t_h) / 2;
        size_t src_stride = s_w * RGB565_BYTES_PER_PIXEL;
        size_t dst_stride = t_w * RGB565_BYTES_PER_PIXEL;

        for (uint16_t y = 0; y < t_h; y++) {
            memcpy(inst->crop_buffer + y * dst_stride,
                   inst->frame_buffer + (off_y + y) * src_stride + off_x * RGB565_BYTES_PER_PIXEL,
                   dst_stride);
        }
        encode_buf = inst->crop_buffer;
        encode_buf_size = inst->crop_buffer_size;
        encode_w = t_w;
        encode_h = t_h;
    } else {
        encode_buf = inst->frame_buffer;
        encode_buf_size = inst->frame_buffer_size;
        encode_w = inst->config.sensor_h_res;
        encode_h = inst->config.sensor_v_res;
    }

    // JPEG encode
    jpeg_encode_cfg_t enc_cfg = {
        .src_type = JPEG_ENCODE_IN_FORMAT_RGB565,
        .sub_sample = JPEG_DOWN_SAMPLING_YUV422,
        .image_quality = inst->config.jpeg_quality,
        .width = encode_w,
        .height = encode_h,
    };

    uint32_t jpeg_size = 0;
    ret = jpeg_encoder_process(inst->jpeg_handle, &enc_cfg,
                               encode_buf, encode_buf_size,
                               inst->jpeg_buffer, inst->jpeg_buffer_size,
                               &jpeg_size);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "JPEG encode failed: %s", esp_err_to_name(ret));
        return ret;
    }

    inst->jpeg_data_size = jpeg_size;
    inst->frame_captured = true;

    // Sync back to caller
    cam->jpeg_buffer = inst->jpeg_buffer;
    cam->jpeg_data_size = inst->jpeg_data_size;
    cam->frame_captured = true;

    return ESP_OK;
}

void csi_camera_free_buffer(csi_camera_t *cam)
{
    s_cam_instance.frame_captured = false;
    s_cam_instance.jpeg_data_size = 0;
    cam->frame_captured = false;
    cam->jpeg_data_size = 0;
}

esp_err_t csi_camera_deinit(csi_camera_t *cam)
{
    csi_camera_t *inst = &s_cam_instance;
    if (!inst->initialized) {
        cam->initialized = false;
        return ESP_OK;
    }

    inst->initialized = false;

    // Disable ISP color if enabled
    if (inst->color.enabled && inst->isp_handle) {
        esp_isp_color_disable(inst->isp_handle);
        inst->color.enabled = false;
    }

    if (inst->sensor_handle) {
        int disable = 0;
        esp_cam_sensor_ioctl(inst->sensor_handle, ESP_CAM_SENSOR_IOC_S_STREAM, &disable);
        esp_cam_sensor_del_dev(inst->sensor_handle);
        inst->sensor_handle = NULL;
    }

    if (inst->cam_handle) {
        esp_cam_ctlr_stop(inst->cam_handle);
        esp_cam_ctlr_disable(inst->cam_handle);
        esp_cam_ctlr_del(inst->cam_handle);
        inst->cam_handle = NULL;
    }

    if (inst->isp_handle) {
        esp_isp_disable(inst->isp_handle);
        esp_isp_del_processor(inst->isp_handle);
        inst->isp_handle = NULL;
    }

    if (inst->jpeg_handle) {
        jpeg_del_encoder_engine(inst->jpeg_handle);
        inst->jpeg_handle = NULL;
    }

    if (inst->sccb_handle) {
        esp_sccb_del_i2c_io(inst->sccb_handle);
        inst->sccb_handle = NULL;
    }

    if (inst->i2c_bus_handle) {
        i2c_del_master_bus(inst->i2c_bus_handle);
        inst->i2c_bus_handle = NULL;
    }

    if (inst->ldo_handle) {
        esp_ldo_release_channel(inst->ldo_handle);
        inst->ldo_handle = NULL;
    }

    if (inst->frame_buffer) {
        heap_caps_free(inst->frame_buffer);
        inst->frame_buffer = NULL;
    }

    if (inst->crop_buffer) {
        heap_caps_free(inst->crop_buffer);
        inst->crop_buffer = NULL;
        inst->crop_buffer_size = 0;
    }

    if (inst->jpeg_buffer) {
        free(inst->jpeg_buffer);
        inst->jpeg_buffer = NULL;
    }

    if (s_frame_ready_sem) {
        vSemaphoreDelete(s_frame_ready_sem);
        s_frame_ready_sem = NULL;
    }

    memset(inst, 0, sizeof(csi_camera_t));
    cam->initialized = false;

    ESP_LOGE(TAG, "camera deinitialized");
    return ESP_OK;
}

// ---- ISP Color Control ----

esp_err_t csi_camera_set_color(csi_camera_t *cam, int brightness, int contrast,
                                int saturation, int hue)
{
    csi_camera_t *inst = &s_cam_instance;
    if (!inst->initialized || !inst->isp_handle) {
        return ESP_ERR_INVALID_STATE;
    }

    // Clamp parameters
    if (brightness < -128) brightness = -128;
    if (brightness > 127) brightness = 127;
    if (contrast < 0) contrast = 0;
    if (contrast > 255) contrast = 255;
    if (saturation < 0) saturation = 0;
    if (saturation > 255) saturation = 255;
    if (hue < 0) hue = 0;
    if (hue > 360) hue = 360;

    // Convert contrast/saturation from 0-255 to fixed-point (1 int bit + 7 dec bits)
    // 0→0.0, 128→1.0, 255→~2.0
    isp_color_contrast_t c_val = {
        .integer = contrast >> 7,           // bit 7 = integer part
        .decimal = contrast & 0x7F,         // bits 0-6 = decimal
    };
    isp_color_saturation_t s_val = {
        .integer = saturation >> 7,
        .decimal = saturation & 0x7F,
    };

    esp_isp_color_config_t color_cfg = {
        .color_contrast = c_val,
        .color_saturation = s_val,
        .color_hue = (uint32_t)hue,
        .color_brightness = brightness,
    };

    esp_err_t ret = esp_isp_color_configure(inst->isp_handle, &color_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "color configure failed: %s", esp_err_to_name(ret));
        return ret;
    }

    if (!inst->color.enabled) {
        ret = esp_isp_color_enable(inst->isp_handle);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "color enable failed: %s", esp_err_to_name(ret));
            return ret;
        }
        inst->color.enabled = true;
    }

    inst->color.brightness = brightness;
    inst->color.contrast = contrast;
    inst->color.saturation = saturation;
    inst->color.hue = hue;

    // Sync to caller
    cam->color = inst->color;

    return ESP_OK;
}

// ---- Mirror / Flip ----

esp_err_t csi_camera_set_mirror_flip(csi_camera_t *cam, bool hmirror, bool vflip)
{
    csi_camera_t *inst = &s_cam_instance;
    if (!inst->initialized || !inst->sensor_handle) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_cam_sensor_device_t *sensor = (esp_cam_sensor_device_t *)inst->sensor_handle;
    esp_err_t ret;
    int val;

    val = hmirror ? 1 : 0;
    ret = esp_cam_sensor_set_para_value(sensor, ESP_CAM_SENSOR_HMIRROR, &val, sizeof(val));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "set hmirror failed: %s", esp_err_to_name(ret));
        return ret;
    }

    val = vflip ? 1 : 0;
    ret = esp_cam_sensor_set_para_value(sensor, ESP_CAM_SENSOR_VFLIP, &val, sizeof(val));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "set vflip failed: %s", esp_err_to_name(ret));
        return ret;
    }

    return ESP_OK;
}
