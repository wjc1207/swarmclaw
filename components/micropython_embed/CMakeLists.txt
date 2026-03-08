cmake_minimum_required(VERSION 3.16)

# Collect all generated C sources
file(GLOB_RECURSE MICROPYTHON_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/py/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/shared/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/port/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/extmod/*.c"
)

idf_component_register(
    SRCS        ${MICROPYTHON_SOURCES}
    INCLUDE_DIRS
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_SOURCE_DIR}/py"
        "${CMAKE_CURRENT_SOURCE_DIR}/genhdr"
        "${CMAKE_CURRENT_SOURCE_DIR}/port"
        "${CMAKE_CURRENT_SOURCE_DIR}/extmod"
    REQUIRES    esp_psram freertos
)

# No target_compile_definitions — mpconfigport.h already defines everything
