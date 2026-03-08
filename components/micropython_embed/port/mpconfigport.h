// MicroPython configuration for EdgeClaw (ESP32-S3 embed)

#include <port/mpconfigport_common.h>

// Enable commonly needed features for IoT scripting
#define MICROPY_CONFIG_ROM_LEVEL        (MICROPY_CONFIG_ROM_LEVEL_BASIC_FEATURES)
#define MICROPY_ENABLE_COMPILER         (1)
#define MICROPY_ENABLE_GC               (1)
#define MICROPY_PY_GC                   (1)
