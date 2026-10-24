# ESP32-CAM: I2S receiver -> Bluetooth A2DP source
#
# The two external components are expected in components/:
#   components/ESP32-A2DP           (this library)
#   components/arduino-audio-tools  (required by ESP32-A2DP's CMakeLists.txt)
# See README.md for the exact commands.
cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/components")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32cam_i2s_a2dp)
