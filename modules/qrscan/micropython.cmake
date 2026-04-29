# qrscan - MicroPython QR code recognition module
#
# Wraps the quirc library (vendored in ./quirc) and exposes a minimal API to
# MicroPython. Decode-time working buffers are allocated via PSRAM-aware
# wrappers (qrscan_alloc.c) by redirecting malloc/calloc/free at compile time
# so quirc.c does not need to be patched.
#
# Usage in Python:
#   import qrscan
#   text = qrscan.scan(gray_bytes, width, height)        # default downsample=True
#   text = qrscan.scan(gray_bytes, width, height, downsample=False)
#   qrscan.version()

get_filename_component(QRSCAN_DIR ${CMAKE_CURRENT_LIST_DIR} ABSOLUTE)

add_library(usermod_qrscan INTERFACE)

# MicroPython binding + helpers
target_sources(usermod_qrscan INTERFACE
    ${QRSCAN_DIR}/modqrscan.c
    ${QRSCAN_DIR}/qrscan_alloc.c
    ${QRSCAN_DIR}/qrscan_resize.c
)

# quirc upstream (vendored, MIT/ISC license)
target_sources(usermod_qrscan INTERFACE
    ${QRSCAN_DIR}/quirc/quirc.c
    ${QRSCAN_DIR}/quirc/decode.c
    ${QRSCAN_DIR}/quirc/identify.c
    ${QRSCAN_DIR}/quirc/version_db.c
)

# Redirect malloc/calloc/free in quirc.c to PSRAM-aware wrappers.
# Only quirc.c performs heap allocations; identify.c, decode.c and
# version_db.c allocate nothing, so scoping the macros to quirc.c is enough.
set_source_files_properties(
    ${QRSCAN_DIR}/quirc/quirc.c
    PROPERTIES COMPILE_DEFINITIONS
    "malloc=qrscan_malloc;calloc=qrscan_calloc;free=qrscan_free"
)

target_include_directories(usermod_qrscan INTERFACE
    ${QRSCAN_DIR}
    ${QRSCAN_DIR}/quirc
)

# qrscan_alloc.c needs esp_heap_caps.h from the heap component
idf_component_get_property(heap_inc heap INCLUDE_DIRS)
if(heap_inc)
    target_include_directories(usermod_qrscan INTERFACE ${heap_inc})
endif()

target_link_libraries(usermod INTERFACE usermod_qrscan)
