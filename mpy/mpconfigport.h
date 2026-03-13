/* mpconfigport.h — MicroPython embed config for ESP32-S3 AI Agent
 *
 * Design goals:
 *   1. Lean RAM footprint (32 KB GC heap on PSRAM)
 *   2. Rich tracebacks for agent self-correction loop
 *   3. Filesystem import so agent can write .py libs to SPIFFS
 *   4. JSON + string ops the agent needs without heavy stdlib
 *   5. Capturable stdout via MP_PLAT_PRINT_STRN hook
 *
 * Deliberately excluded:
 *   math     — causes MemoryError on 32 KB embed heap
 *   uasyncio — use FreeRTOS tasks instead
 *   network  — not meaningful in embed context
 */

#ifndef MICROPY_INCLUDED_COMPONENTS_MICROPYTHON_EMBED_MPCONFIGPORT_H
#define MICROPY_INCLUDED_COMPONENTS_MICROPYTHON_EMBED_MPCONFIGPORT_H

#include <stddef.h>

#include <port/mpconfigport_common.h>

/* ── ROM level ────────────────────────────────────────────────────────────
 * MINIMUM = clean slate; we opt in to exactly what we need.
 * Prevents surprise heavy modules being pulled in automatically.        */
#define MICROPY_CONFIG_ROM_LEVEL                (MICROPY_CONFIG_ROM_LEVEL_MINIMUM)

/* ── Architecture — Xtensa LX7 (ESP32-S3) ────────────────────────────────
 * LX7 has no dedicated NLR support; setjmp/longjmp required.
 * Both must be set together for correct GC register scanning.           */
#define MICROPY_NLR_SETJMP                      (1)
#define MICROPY_GCREGS_SETJMP                   (1)

/* ── Core runtime ─────────────────────────────────────────────────────── */
#define MICROPY_ENABLE_COMPILER                 (1)  /* compile .py at runtime */
#define MICROPY_ENABLE_GC                       (1)  /* garbage collector       */
#define MICROPY_PY_GC                           (1)  /* gc module               */
#define MICROPY_STACK_CHECK                     (0)  /* FreeRTOS task stack top unreliable;
                                                        mp_stack_set_limit() called manually in mpy_runner.c */
#define MICROPY_ENABLE_EMERGENCY_EXCEPTION_BUF  (1)
#define MICROPY_EMERGENCY_EXCEPTION_BUF_SIZE    (64)

/* ── Integer implementation ───────────────────────────────────────────── *
 * MPZ = arbitrary precision. Agents commonly produce large intermediate  *
 * ints (timestamps, bitmasks). Lower overhead than LONGLONG.             */
#define MICROPY_LONGINT_IMPL                    (MICROPY_LONGINT_IMPL_MPZ)

/* ── Float ────────────────────────────────────────────────────────────── *
 * Single precision saves ~4 KB vs double. Sufficient for sensor scaling, *
 * PWM duty, LED colour math.                                             *
 * DO NOT enable MICROPY_PY_MATH — causes MemoryError on 32 KB heap.     *
 * Provide math helpers as .py files on SPIFFS instead.                  */
#define MICROPY_FLOAT_IMPL                      (MICROPY_FLOAT_IMPL_FLOAT)

/* ── Filesystem import — CRITICAL for agent .py lib loading ──────────── *
 * Enables:  import mylib   (from /spiffs/mylib.py written by the agent)  *
 * Without this the agent is locked to the fixed C module surface.       */
#define MICROPY_ENABLE_EXTERNAL_IMPORT          (1)
#define MICROPY_READER_POSIX                    (1)  /* POSIX fopen() for SPIFFS */

/* ── Exception detail — CRITICAL for agent self-correction loop ───────── *
 * Full tracebacks with file + line numbers allow the agent to parse      *
 * errors and rewrite the script autonomously.                            */
#define MICROPY_ENABLE_SOURCE_LINE              (1)
#define MICROPY_ERROR_REPORTING                 (MICROPY_ERROR_REPORTING_DETAILED)
#define MICROPY_ERRORS_INCLUDE_FILENAME         (1)
#define MICROPY_PY_BUILTINS_NOTIMPLEMENTED      (1)
#define MICROPY_PY_BUILTINS_ISINSTANCE_MULTI_ARG (1)

/* ── String & bytes ───────────────────────────────────────────────────── */
#define MICROPY_PY_BUILTINS_STR_UNICODE         (1)  /* UTF-8 strings          */
#define MICROPY_PY_BUILTINS_BYTEARRAY           (1)  /* I2C/SPI byte buffers   */
#define MICROPY_PY_BUILTINS_MEMORYVIEW          (1)  /* zero-copy buffer views */
#define MICROPY_PY_BUILTINS_STR_OP_MODULO       (1)  /* "fmt" % (val,) syntax  */

/* ── Built-in data structures ─────────────────────────────────────────── */
#define MICROPY_PY_BUILTINS_SET                 (1)
#define MICROPY_PY_COLLECTIONS                  (1)  /* OrderedDict, namedtuple */

/* ── I/O ──────────────────────────────────────────────────────────────── *
 * open() is provided by mp_builtin_open in main/mpy/mpy_port_io.c.       */
#define MICROPY_PY_IO                           (1)
#define MICROPY_PY_IO_FILEIO                    (1)

/* ── sys module ───────────────────────────────────────────────────────── */
#define MICROPY_PY_SYS                          (1)
#define MICROPY_PY_SYS_PLATFORM                 "esp32s3"

/* ── Agent-essential stdlib ───────────────────────────────────────────── *
 * json     — parse tool call results, build structured output            *
 * re       — pattern-match captured output in the agent loop             *
 * binascii — base64 / hex encoding for SPI/I2C binary data              *
 * struct   — pack/unpack raw sensor byte streams                         */
#define MICROPY_PY_JSON                         (1)
#define MICROPY_PY_RE                           (1)
#define MICROPY_PY_BINASCII                     (1)
#define MICROPY_PY_STRUCT                       (1)

/* ── Script runtime essentials for on-device agent workflows ─────────── *
 * os       — list/remove/rename files under /spiffs                      *
 * time     — sleep/ticks/timeouts for retry/backoff loops                *
 * random   — provided by MicroPython core module                          *
 * errno    — stable error codes for robust exception handling             *
 * array    — compact numeric buffers with lower RAM overhead              */
#define MICROPY_PY_OS                           (1)
#define MICROPY_PY_OS_STATVFS                   (1)
#define MICROPY_PY_TIME                         (1)
#define MICROPY_PY_TIME_GMTIME_LOCALTIME_MKTIME (1)
#define MICROPY_PY_TIME_TIME_TIME_NS            (1)
#define MICROPY_PY_RANDOM                       (1)
#define MICROPY_PY_RANDOM_EXTRA_FUNCS           (1)
#define MICROPY_PY_ERRNO                        (1)
#define MICROPY_PY_ERRNO_ERRORCODE              (1)
#define MICROPY_PY_ARRAY                        (1)

/* ── Builtins the agent will use naturally ────────────────────────────── */
#define MICROPY_PY_BUILTINS_ENUMERATE           (1)
#define MICROPY_PY_BUILTINS_FILTER              (1)
#define MICROPY_PY_BUILTINS_MAP                 (1)
#define MICROPY_PY_BUILTINS_ZIP                 (1)
#define MICROPY_PY_BUILTINS_REVERSED            (1)
#define MICROPY_PY_BUILTINS_SORTED              (1)

/* ── Deliberately NOT enabled ─────────────────────────────────────────── *
 *   MICROPY_PY_MATH     — MemoryError on 32 KB heap; use inline helpers  *
 *   MICROPY_PY_UASYNCIO — use FreeRTOS tasks instead                     *
 *   MICROPY_PY_NETWORK  — not in embed context                           *
 *   MICROPY_PY_USSL     — not in embed context                           */
#define MICROPY_PY_MATH                         (1)
#define MICROPY_PY_UASYNCIO                     (0)
#define MICROPY_PY_NETWORK                      (0)
#define MICROPY_PY_USSL                         (0)

/* ── Stdout capture ───────────────────────────────────────────────────── *
 * Overrides the default mp_hal_stdout_tx_strn_cooked → printf path.     *
 * mpy_stdout_write() dispatches to the capture hook installed by         *
 * mpy_runner.c before mp_embed_init(). mphalport.c/h untouched.         */
#ifdef __cplusplus
extern "C" {
#endif

void mpy_stdout_write(const char *str, size_t len);

#define MP_PLAT_PRINT_STRN(str, len)            mpy_stdout_write((str), (len))

#ifdef __cplusplus
}
#endif

#endif // MICROPY_INCLUDED_COMPONENTS_MICROPYTHON_EMBED_MPCONFIGPORT_H