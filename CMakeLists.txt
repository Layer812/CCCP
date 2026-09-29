cmake_minimum_required(VERSION 3.16)
set(IDF_TARGET "esp32s3" CACHE STRING "" FORCE)

# CCCP R1S intentionally uses Arduino-ESP32's packaged generic ESP32-S3
# variant under ESP-IDF.  The component-manager package does not ship the
# PlatformIO-only m5stack_stamp_s3 variant directory.
# M5GFX performs Cardputer/ADV display detection from the real GPIO/SPI/I2C
# hardware and does not require the Arduino board variant for that probe.
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(cccp_r1)
