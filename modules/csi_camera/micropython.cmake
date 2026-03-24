# csi_camera - MicroPython CSI Camera module for ESP32-P4
#
# Wraps ESP-IDF MIPI CSI camera driver + ISP + HW JPEG encoder,
# providing JPEG capture capability for OV5647 and other MIPI CSI sensors.
#
# Dependencies (via idf_component.yml):
#   - espressif/esp_cam_sensor (OV5647 auto-detection + SCCB/I2C)
#   - ESP-IDF built-in: esp_driver_cam, driver/jpeg, driver/isp
#
# Usage in Python:
#   from csi_camera import CSICamera
#   cam = CSICamera(h_res=800, v_res=640, jpeg_quality=80)
#   cam.init()
#   img = cam.capture()  # JPEG bytes
#   cam.free_buffer()
#   cam.deinit()

get_filename_component(CSI_CAMERA_DIR ${CMAKE_CURRENT_LIST_DIR} ABSOLUTE)

# Create INTERFACE library for MicroPython usermod system
add_library(usermod_csi_camera INTERFACE)

# Source files
target_sources(usermod_csi_camera INTERFACE
    ${CSI_CAMERA_DIR}/csi_camera.c
    ${CSI_CAMERA_DIR}/csi_camera_hal.c
)

# Include directory for csi_camera_hal.h
target_include_directories(usermod_csi_camera INTERFACE
    ${CSI_CAMERA_DIR}
)

# Link ESP-IDF components

# esp_cam_sensor (installed via idf_component.yml as espressif__esp_cam_sensor)
idf_component_get_property(cam_sensor_dir espressif__esp_cam_sensor COMPONENT_DIR)
idf_component_get_property(cam_sensor_lib espressif__esp_cam_sensor COMPONENT_LIB)
target_include_directories(usermod_csi_camera INTERFACE
    ${cam_sensor_dir}/include
)
target_link_libraries(usermod_csi_camera INTERFACE ${cam_sensor_lib})

# ESP-IDF built-in components: esp_driver_cam, esp_driver_jpeg, esp_driver_isp
foreach(comp esp_driver_cam esp_driver_jpeg esp_driver_isp esp_driver_i2c)
    idf_component_get_property(${comp}_lib ${comp} COMPONENT_LIB)
    target_link_libraries(usermod_csi_camera INTERFACE ${${comp}_lib})
endforeach()

# LDO regulator is part of esp_hw_support
idf_component_get_property(hw_support_lib esp_hw_support COMPONENT_LIB)
target_link_libraries(usermod_csi_camera INTERFACE ${hw_support_lib})

# Link to the main usermod target
target_link_libraries(usermod INTERFACE usermod_csi_camera)
