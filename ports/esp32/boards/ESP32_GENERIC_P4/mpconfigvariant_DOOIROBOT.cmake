set(IDF_TARGET esp32p4)

set(SDKCONFIG_DEFAULTS
    boards/sdkconfig.base
    boards/sdkconfig.p4
    boards/sdkconfig.p4_wifi_common
    boards/sdkconfig.p4_wifi_dooirobot
)

list(APPEND MICROPY_DEF_BOARD
    MICROPY_HW_BOARD_NAME="DooiRobot ESP32P4 with C6 WiFi"
    MICROPY_PY_NETWORK_WLAN=1
    MICROPY_PY_BLUETOOTH=1
)
