/*
 * csi_camera_hal.h
 *
 * HAL layer for ESP32-P4 MIPI CSI camera with hardware JPEG encoding.
 * Uses singleton pattern — hardware is initialized once and reused across
 * Python object lifetimes (handles script restart without explicit deinit).
 */

#ifndef CSI_CAMERA_HAL_H
#define CSI_CAMERA_HAL_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

typedef struct csi_camera_config {
    uint16_t h_res;             // Target horizontal resolution (e.g. 320)
    uint16_t v_res;             // Target vertical resolution (e.g. 240)
    uint16_t sensor_h_res;      // Actual sensor output h_res (set during init)
    uint16_t sensor_v_res;      // Actual sensor output v_res (set during init)
    uint8_t  jpeg_quality;      // JPEG quality 1-100
    uint8_t  data_lanes;        // Number of MIPI CSI data lanes (1 or 2)
    uint16_t lane_bitrate_mbps; // Lane bitrate in Mbps
    int8_t   sccb_sda_io;      // SCCB SDA GPIO (-1 for default)
    int8_t   sccb_scl_io;      // SCCB SCL GPIO (-1 for default)
} csi_camera_config_t;

typedef struct csi_camera_color {
    int  brightness;    // -128 to 127, default 0
    int  contrast;      // 0 to 255, maps to 0.0~2.0 (128=1.0), default 128
    int  saturation;    // 0 to 255, maps to 0.0~2.0 (128=1.0), default 128
    int  hue;           // 0 to 360 degrees, default 0
    bool enabled;       // Whether ISP color processing is active
} csi_camera_color_t;

typedef struct csi_camera {
    csi_camera_config_t config;
    csi_camera_color_t color;
    void *cam_handle;       // esp_cam_ctlr_handle_t
    void *isp_handle;       // isp_proc_handle_t
    void *jpeg_handle;      // jpeg_encoder_handle_t
    void *sccb_handle;      // esp_sccb_io_handle_t
    void *i2c_bus_handle;   // i2c_master_bus_handle_t
    void *ldo_handle;       // esp_ldo_channel_handle_t
    void *sensor_handle;    // esp_cam_sensor_device_t *
    uint8_t *frame_buffer;  // RGB frame buffer (PSRAM, sensor resolution)
    size_t frame_buffer_size;
    uint8_t *crop_buffer;   // Cropped RGB buffer (if crop needed)
    size_t crop_buffer_size;
    uint8_t *jpeg_buffer;   // JPEG output buffer
    size_t jpeg_buffer_size;
    size_t jpeg_data_size;  // Actual JPEG data size after encoding
    int init_step;          // Last completed init step (for debugging)
    bool initialized;
    bool frame_captured;
    bool needs_crop;        // True if target != sensor resolution
} csi_camera_t;

// Get singleton instance (creates if needed, never returns NULL)
csi_camera_t *csi_camera_get_instance(void);

// Initialize camera — reuses existing hardware if already initialized with compatible config
esp_err_t csi_camera_init(csi_camera_t *cam);

// Capture one frame and encode to JPEG
esp_err_t csi_camera_capture(csi_camera_t *cam);

// Free the captured frame buffer
void csi_camera_free_buffer(csi_camera_t *cam);

// Deinitialize and release all resources
esp_err_t csi_camera_deinit(csi_camera_t *cam);

// Set ISP color parameters (can be called anytime after init)
esp_err_t csi_camera_set_color(csi_camera_t *cam, int brightness, int contrast,
                                int saturation, int hue);

// Set mirror/flip (can be called anytime after init)
esp_err_t csi_camera_set_mirror_flip(csi_camera_t *cam, bool hmirror, bool vflip);

#endif // CSI_CAMERA_HAL_H
