# MicroPython ml307 module CMake configuration
#
# This file integrates the esp-ml307 C++ library as a MicroPython user module.
# It includes the C++ wrapper layer and MicroPython bindings.
#
# Usage:
#   Build with: make USER_C_MODULES=/path/to/modules/ml307mod/micropython.cmake
#   Or add to your board's mpconfigboard.cmake
#
# Supported modules: ML307R, ML307A, EC801E, NT26K
# Features: HTTP/HTTPS, WebSocket, TCP/SSL, UDP, MQTT

# Get the directory containing this file
get_filename_component(ML307_MOD_DIR ${CMAKE_CURRENT_LIST_DIR} ABSOLUTE)

# Create INTERFACE library for usermod system
add_library(usermod_ml307 INTERFACE)

# Add MicroPython binding sources (C module and C++ wrapper)
target_sources(usermod_ml307 INTERFACE
    ${ML307_MOD_DIR}/ml307mod.c
    ${ML307_MOD_DIR}/ml307call.cpp
)

# Include directories for our module
target_include_directories(usermod_ml307 INTERFACE
    ${ML307_MOD_DIR}
)

# Get esp-ml307 component include directories
idf_component_get_property(ml307_dir esp-ml307 COMPONENT_DIR)
target_include_directories(usermod_ml307 INTERFACE
    ${ml307_dir}/include
)

# Link esp-ml307 component
idf_component_get_property(ml307_lib esp-ml307 COMPONENT_LIB)
target_link_libraries(usermod_ml307 INTERFACE ${ml307_lib})

# Link to the main usermod target
target_link_libraries(usermod INTERFACE usermod_ml307)

