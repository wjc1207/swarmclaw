/* qstrdefsport.h — custom QSTR definitions for swarmclaw hardware modules
 *
 * Place at: swarmclaw/main/mpy/qstrdefsport.h
 * Reference in embed.mk via: QSTR_DEFS = $(PROJECT_DIR)/main/mpy/qstrdefsport.h
 *
 * Every MP_QSTR_xxx not in MicroPython's py/qstrdefs.h must appear here.
 */

/* ── Hardware module names ─────────────────────────────────── */
Q(gpio)
Q(i2c)
Q(spi)
Q(rgb)
Q(pwm)
Q(time_ms)

/* ── Hardware module methods ───────────────────────────────── */
Q(write)
Q(read)
Q(transfer)
Q(fill)
Q(show)
Q(start)
Q(sleep)

/* ── File I/O (mpy_file_type / mp_builtin_open) ───────────── */
Q(file)
Q(seek)
Q(tell)
Q(flush)
Q(close)
Q(readlines)
Q(__del__)
Q(__enter__)
Q(__exit__)

/* ── Random (for jitter, sampling, non-deterministic branch choices) ── */
Q(cmath)