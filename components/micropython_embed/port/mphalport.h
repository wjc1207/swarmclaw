// Define so there's no dependency on extmod/virtpin.h
#define mp_hal_pin_obj_t

#include <stddef.h>

/**
 * Hook for redirecting MicroPython stdout output.
 * When non-NULL, all print() / exception output is sent here
 * instead of printf.  Set by mpy_runner before executing scripts.
 */
typedef void (*mpy_stdout_hook_t)(const char *str, size_t len);
void mpy_set_stdout_hook(mpy_stdout_hook_t hook);
