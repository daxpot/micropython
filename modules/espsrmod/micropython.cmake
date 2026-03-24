# ESP-SR Wake Word Detection Module for MicroPython
# This module provides wake word detection using Espressif's esp-sr library.
#
# Usage:
#   import espsr
#   wn = espsr.WakeNet(callback=on_wakeup)
#   wn.resume(i2s_id=0, sck=12, ws=13, sd=14, pdm=True)

# Get the directory containing this file
get_filename_component(ESPSR_MOD_DIR ${CMAKE_CURRENT_LIST_DIR} ABSOLUTE)

# Create INTERFACE library for usermod system
add_library(usermod_espsr INTERFACE)

# Add MicroPython binding sources
target_sources(usermod_espsr INTERFACE
    ${ESPSR_MOD_DIR}/espsrmod.c
    ${ESPSR_MOD_DIR}/espsr_audio.c
)

# Include directories for our module
target_include_directories(usermod_espsr INTERFACE
    ${ESPSR_MOD_DIR}
)

# Get esp-sr component include directories and link library
# esp-sr is added via idf_component.yml, so we use idf_component_get_property
idf_component_get_property(espsr_dir espressif__esp-sr COMPONENT_DIR)
target_include_directories(usermod_espsr INTERFACE
    ${espsr_dir}/include
    ${espsr_dir}/src/include
)

idf_component_get_property(espsr_lib espressif__esp-sr COMPONENT_LIB)
target_link_libraries(usermod_espsr INTERFACE ${espsr_lib})

# Link to the main usermod target
target_link_libraries(usermod INTERFACE usermod_espsr)
