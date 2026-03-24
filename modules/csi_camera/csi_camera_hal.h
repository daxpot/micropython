/*
 * csi_camera_hal.h
 *
 * HAL layer for ESP32-P4 MIPI CSI camera with hardware JPEG encoding.
 * Wraps ESP-IDF esp_cam_ctlr_csi + ISP + JPEG encoder APIs.
 */

#ifndef CSI_CAMERA_HAL_H
#define CSI_CAMERA_HAL_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

typedef struct csi_camera_config {
    uint16_t h_res;             // Horizontal resolution (e.g. 800)
    uint16_t v_res;             // Vertical resolution (e.g. 640)
    uint8_t  jpeg_quality;      // JPEG quality 1-100
    uint8_t  data_lanes;        // Number of MIPI CSI data lanes (1 or 2)
    uint16_t lane_bitrate_mbps; // Lane bitrate in Mbps
    int8_t   sccb_sda_io;      // SCCB SDA GPIO (-1 for default)
    int8_t   sccb_scl_io;      // SCCB SCL GPIO (-1 for default)
} csi_camera_config_t;

typedef struct csi_camera {
    csi_camera_config_t config;
    void *cam_handle;       // esp_cam_ctlr_handle_t
    void *isp_handle;       // isp_proc_handle_t
    void *jpeg_handle;      // jpeg_encoder_handle_t
    void *sccb_handle;      // esp_sccb_io_handle_t
    void *i2c_bus_handle;   // i2c_master_bus_handle_t
    void *ldo_handle;       // esp_ldo_channel_handle_t
    void *sensor_handle;    // esp_cam_sensor_device_t *
    uint8_t *frame_buffer;  // RGB frame buffer (PSRAM)
    size_t frame_buffer_size;
    uint8_t *jpeg_buffer;   // JPEG output buffer
    size_t jpeg_buffer_size;
    size_t jpeg_data_size;  // Actual JPEG data size after encoding
    int init_step;          // Last completed init step (for debugging)
    bool initialized;
    bool frame_captured;
} csi_camera_t;

// Initialize camera (CSI + sensor + ISP + JPEG encoder)
esp_err_t csi_camera_init(csi_camera_t *cam);

// Capture one frame and encode to JPEG
// After calling, cam->jpeg_buffer contains JPEG data of cam->jpeg_data_size bytes
esp_err_t csi_camera_capture(csi_camera_t *cam);

// Free the captured frame buffer
void csi_camera_free_buffer(csi_camera_t *cam);

// Deinitialize and release all resources
esp_err_t csi_camera_deinit(csi_camera_t *cam);

#endif // CSI_CAMERA_HAL_H
