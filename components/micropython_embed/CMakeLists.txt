cmake_minimum_required(VERSION 3.16)

# Collect all generated C sources
file(GLOB_RECURSE MICROPYTHON_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/py/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/shared/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/port/*.c"
)

idf_component_register(
    SRCS        ${MICROPYTHON_SOURCES}
    INCLUDE_DIRS
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_SOURCE_DIR}/genhdr"
        "${CMAKE_CURRENT_SOURCE_DIR}/port"
    REQUIRES    esp_psram freertos
)

# MicroPython needs these compile flags
target_compile_definitions(${COMPONENT_LIB} PUBLIC
    MICROPY_NLR_SETJMP=1          # use setjmp for exception handling (ESP32 safe)
    MICROPY_ENABLE_GC=1
    MICROPY_ENABLE_COMPILER=1     # include source compiler (agent writes .py text)
)
