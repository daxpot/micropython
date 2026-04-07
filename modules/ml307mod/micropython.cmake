# ml307mod - ML307R 4G Modem C Extension for MicroPython
#
# High-performance AT command engine with UHCI DMA background task.
# Provides _ml307 Python module for socket operations over 4G.
#
# Usage in Python:
#   import _ml307
#   _ml307.init(tx=12, rx=11, baudrate=921600)

get_filename_component(ML307_MOD_DIR ${CMAKE_CURRENT_LIST_DIR} ABSOLUTE)

add_library(usermod_ml307 INTERFACE)

target_sources(usermod_ml307 INTERFACE
    ${ML307_MOD_DIR}/ml307_mod.c
    ${ML307_MOD_DIR}/ml307_at.cc
)

target_include_directories(usermod_ml307 INTERFACE
    ${ML307_MOD_DIR}
)

# Link ESP-IDF UART driver
idf_component_get_property(driver_lib driver COMPONENT_LIB)
target_link_libraries(usermod_ml307 INTERFACE ${driver_lib})

# Link uart-uhci component for UHCI DMA (ESP32-S3/C3/C6/P4 only)
if(TARGET __idf_78__uart-uhci)
    idf_component_get_property(uart_uhci_lib 78__uart-uhci COMPONENT_LIB)
    target_link_libraries(usermod_ml307 INTERFACE ${uart_uhci_lib})
    target_compile_definitions(usermod_ml307 INTERFACE ML307_USE_UHCI_DMA=1)
endif()

target_link_libraries(usermod INTERFACE usermod_ml307)
