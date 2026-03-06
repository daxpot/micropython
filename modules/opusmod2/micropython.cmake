# opusmod2 - MicroPython Opus module using 78/esp-opus-encoder
#
# This module wraps the esp-opus-encoder C++ library via a C bridge layer,
# providing Opus encode/decode/resample capabilities to MicroPython.
#
# Dependencies:
#   - 78/esp-opus-encoder ^2.4.1 (via idf_component.yml)
#   - 78/esp-opus ^1.0.5 (transitive dependency, includes Xtensa LX7 optimizations)
#
# Usage in Python:
#   import opusmod2
#   enc = opusmod2.Encoder(sample_rate=16000, channels=1, duration_ms=60)
#   dec = opusmod2.Decoder(sample_rate=16000, channels=1, duration_ms=60)
#   res = opusmod2.Resampler(input_rate=48000, output_rate=16000)

get_filename_component(OPUSMOD2_DIR ${CMAKE_CURRENT_LIST_DIR} ABSOLUTE)

# Create INTERFACE library for MicroPython usermod system
add_library(usermod_opusmod2 INTERFACE)

# Add C source (MicroPython binding)
target_sources(usermod_opusmod2 INTERFACE
    ${OPUSMOD2_DIR}/opusmod2.c
)

# Add C++ source (bridge layer)
# Note: CMake handles .cpp files with C++ compiler automatically
target_sources(usermod_opusmod2 INTERFACE
    ${OPUSMOD2_DIR}/opusmod2_wrapper.cpp
)

# Include our own directory for opusmod2_wrapper.h
target_include_directories(usermod_opusmod2 INTERFACE
    ${OPUSMOD2_DIR}
)

# Get esp-opus-encoder component (installed via idf_component.yml)
# The component name in the registry is "78/esp-opus-encoder",
# which IDF component manager installs as "78__esp-opus-encoder"
idf_component_get_property(opus_enc_dir 78__esp-opus-encoder COMPONENT_DIR)
target_include_directories(usermod_opusmod2 INTERFACE
    ${opus_enc_dir}/include
)

idf_component_get_property(opus_enc_lib 78__esp-opus-encoder COMPONENT_LIB)
target_link_libraries(usermod_opusmod2 INTERFACE ${opus_enc_lib})

# Also link the underlying esp-opus library for opus.h and resampler headers
idf_component_get_property(opus_dir 78__esp-opus COMPONENT_DIR)
target_include_directories(usermod_opusmod2 INTERFACE
    ${opus_dir}/include
    ${opus_dir}/silk          # for resampler_structs.h (needed by opus_resampler.h)
    ${opus_dir}/silk/fixed
    ${opus_dir}/celt
    ${opus_dir}/src
    ${opus_dir}               # for config.h if needed
)

idf_component_get_property(opus_lib 78__esp-opus COMPONENT_LIB)
target_link_libraries(usermod_opusmod2 INTERFACE ${opus_lib})

# Link to the main usermod target
target_link_libraries(usermod INTERFACE usermod_opusmod2)
