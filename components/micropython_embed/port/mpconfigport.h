/* MicroPython embed port configuration for EdgeClaw (ESP32-S3) */

/* Enable the compiler so mp_embed_exec_str() works */
#define MICROPY_ENABLE_COMPILER             (1)

/* Enable GC so we get a working heap */
#define MICROPY_ENABLE_GC                   (1)

/* Basic features */
#define MICROPY_PY_GC                       (1)
#define MICROPY_PY_SYS                      (1)
#define MICROPY_PY_IO                       (1)
#define MICROPY_PY_MATH                     (1)
#define MICROPY_FLOAT_IMPL                  (MICROPY_FLOAT_IMPL_FLOAT)
#define MICROPY_ERROR_REPORTING             (MICROPY_ERROR_REPORTING_DETAILED)

/* Include the common embed port configuration */
#include "port/mpconfigport_common.h"
