# ESP32-WROVER: Internet radio (HTTP/HTTPS MP3 stream) -> Bluetooth A2DP source
#
# ESP32-A2DP and its arduino-audio-tools dependency are the same vendored
# components the I2S bridge uses. Sharing them keeps both projects on one
# library version and avoids storing a second ~120 MB copy in the repository.
cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/../esp32cam_i2s_a2dp/components")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32radio_a2dp)
