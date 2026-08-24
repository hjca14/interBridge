# Used only by the esp32-c3-si3050-clock-probe-idf5 PlatformIO environment
# (framework = espidf). PlatformIO's `build_src_filter` (used by every
# Arduino-framework environment in platformio.ini to isolate each bench
# entry point) has NO EFFECT for `framework = espidf` - without this
# file, PlatformIO auto-generates a CMakeLists.txt that globs and
# compiles every file under src/ as the "main" component, which pulls in
# Arduino-only files (Arduino.h, ArduinoJson.h, etc.) that cannot compile
# under ESP-IDF. This file is the ESP-IDF-native equivalent isolation
# mechanism: it lists the exact single source file for the IDF5 TDM
# probe investigation and nothing else.
#
# Arduino-framework environments (esp32-c3, esp32-c3-dev-mqtt,
# esp32-c3-si3050-clock-probe, esp32dev-si3050-clock-meter) and the
# native test environment do not use CMake at all and are unaffected by
# this file's presence.
idf_component_register(
    SRCS "dev/si3050_clock_probe_generator_idf5_main.cpp"
    INCLUDE_DIRS "."
)
