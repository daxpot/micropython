# p4_modules.cmake
# Wrapper to include MicroPython user C modules compatible with ESP32-P4.
# Used via: USER_C_MODULES=../../../modules/p4_modules.cmake

include(${CMAKE_CURRENT_LIST_DIR}/opusmod2/micropython.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/csi_camera/micropython.cmake)
