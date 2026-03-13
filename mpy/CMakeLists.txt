# components/micropython_embed/CMakeLists.txt

cmake_minimum_required(VERSION 3.16)

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
)

# Suppress warnings in generated MicroPython core code
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error -w)

# Prevent conflict with newlib's __assert_func on ESP-IDF
target_compile_definitions(${COMPONENT_LIB} PRIVATE NDEBUG)
