# p4_modules.cmake
# Wrapper to include MicroPython user C modules compatible with ESP32-P4.
# Used via: USER_C_MODULES=../../../modules/p4_modules.cmake
include(${CMAKE_CURRENT_LIST_DIR}/ml307mod/micropython.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/espsrmod/micropython.cmake)

include(${CMAKE_CURRENT_LIST_DIR}/opusmod2/micropython.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/csi_camera/micropython.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/mp_jpeg/micropython.cmake)