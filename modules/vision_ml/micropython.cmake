# vision_ml - On-device image classification for ESP32-P4
#
# Loads a pre-flashed MobileNetV1 0.25_224 backbone from the ml_model raw
# partition (subtype 0x40 @ 0xF00000), runs TFLite Micro inference, and
# applies a small Dense+ReLU+Softmax head whose weights are loaded from
# /models/<name>/manifest.json + head_weights.bin (exported by the browser
# trainer at lerobot-web).
#
# Dependencies (via idf_component.yml):
#   - espressif/esp-tflite-micro
# Built-in components: cJSON, esp_partition, spi_flash, esp_psram

get_filename_component(VISION_ML_DIR ${CMAKE_CURRENT_LIST_DIR} ABSOLUTE)

if(IDF_TARGET STREQUAL "esp32p4" OR IDF_TARGET STREQUAL "esp32s3")

add_library(usermod_vision_ml INTERFACE)

target_sources(usermod_vision_ml INTERFACE
    ${VISION_ML_DIR}/vision_ml.c
    ${VISION_ML_DIR}/vision_ml_head.c
    ${VISION_ML_DIR}/vision_ml_manifest.c
    ${VISION_ML_DIR}/vision_ml_engine.cpp
)

target_include_directories(usermod_vision_ml INTERFACE
    ${VISION_ML_DIR}
)

# C++ flags: TFLM is C++17
set_source_files_properties(${VISION_ML_DIR}/vision_ml_engine.cpp
    PROPERTIES COMPILE_OPTIONS "-std=gnu++17;-fno-exceptions;-fno-rtti;-Wno-unused-parameter")

# Link esp-tflite-micro managed component
idf_component_get_property(tflm_dir espressif__esp-tflite-micro COMPONENT_DIR)
idf_component_get_property(tflm_lib espressif__esp-tflite-micro COMPONENT_LIB)
target_include_directories(usermod_vision_ml INTERFACE
    ${tflm_dir}
    ${tflm_dir}/tensorflow
    ${tflm_dir}/third_party/flatbuffers/include
    ${tflm_dir}/third_party/gemmlowp
    ${tflm_dir}/third_party/kissfft
)
target_link_libraries(usermod_vision_ml INTERFACE ${tflm_lib})

# ESP-IDF built-ins we use directly
foreach(comp json esp_partition spi_flash esp_psram)
    idf_component_get_property(${comp}_lib ${comp} COMPONENT_LIB)
    if(${comp}_lib)
        target_link_libraries(usermod_vision_ml INTERFACE ${${comp}_lib})
    endif()
endforeach()

target_link_libraries(usermod INTERFACE usermod_vision_ml)

endif()
