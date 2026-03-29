# ESP-SR Wake Word Detection Module for MicroPython (Feed Mode)
#
# Usage:
#   import espsrmod
#   wn = espsrmod.WakeNet()
#   result = wn.detect(pcm_buf)

get_filename_component(ESPSRMOD_DIR ${CMAKE_CURRENT_LIST_DIR} ABSOLUTE)

# ESP-SR is only available on ESP32-S3 and ESP32-P4
if(IDF_TARGET STREQUAL "esp32s3" OR IDF_TARGET STREQUAL "esp32p4")

add_library(usermod_espsrmod INTERFACE)

target_sources(usermod_espsrmod INTERFACE
    ${ESPSRMOD_DIR}/espsrmod.c
    ${ESPSRMOD_DIR}/wakenet_ops.c
)

target_include_directories(usermod_espsrmod INTERFACE
    ${ESPSRMOD_DIR}
)

# Link esp-sr managed component
idf_component_get_property(espsr_dir espressif__esp-sr COMPONENT_DIR)
target_include_directories(usermod_espsrmod INTERFACE
    ${espsr_dir}/include
    ${espsr_dir}/src/include
)

idf_component_get_property(espsr_lib espressif__esp-sr COMPONENT_LIB)
target_link_libraries(usermod_espsrmod INTERFACE ${espsr_lib})

target_link_libraries(usermod INTERFACE usermod_espsrmod)

endif()
