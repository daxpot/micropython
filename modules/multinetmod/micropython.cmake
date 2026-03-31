# MultiNet Speech Command Recognition Module for MicroPython
#
# Provides runtime-configurable wake word / command recognition via pinyin.
#
# Usage:
#   import multinetmod
#   mn = multinetmod.MultiNet()
#   mn.add_command(1, "xiao yu tong xue")
#   mn.update()
#   result = mn.detect(pcm_buf)

get_filename_component(MULTINETMOD_DIR ${CMAKE_CURRENT_LIST_DIR} ABSOLUTE)

# MultiNet is only available on ESP32-S3 and ESP32-P4
if(IDF_TARGET STREQUAL "esp32s3" OR IDF_TARGET STREQUAL "esp32p4")

add_library(usermod_multinetmod INTERFACE)

target_sources(usermod_multinetmod INTERFACE
    ${MULTINETMOD_DIR}/multinetmod.c
    ${MULTINETMOD_DIR}/multinet_ops.c
)

target_include_directories(usermod_multinetmod INTERFACE
    ${MULTINETMOD_DIR}
)

# Link esp-sr managed component (same as espsrmod)
idf_component_get_property(espsr_dir espressif__esp-sr COMPONENT_DIR)
target_include_directories(usermod_multinetmod INTERFACE
    ${espsr_dir}/include
    ${espsr_dir}/src/include
)

idf_component_get_property(espsr_lib espressif__esp-sr COMPONENT_LIB)
target_link_libraries(usermod_multinetmod INTERFACE ${espsr_lib})

target_link_libraries(usermod INTERFACE usermod_multinetmod)

endif()
