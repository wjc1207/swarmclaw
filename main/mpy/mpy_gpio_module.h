#pragma once

/**
 * MicroPython hardware-binding modules.
 *
 * Call mpy_gpio_modules_register() after mp_embed_init() so every
 * agent-written script can:
 *   import gpio    # gpio.write(pin, val), gpio.read(pin), gpio.set_dir(pin, "in"/"out")
 *   import i2c     # i2c.write(sda, scl, addr, data, freq), i2c.read(...)
 *   import spi     # spi.transfer(mosi, miso, sclk, cs, tx)
 *   import rgb     # rgb.fill(pin, n, r, g, b), rgb.show(pin, n)
 *   import pwm     # pwm.start(pin, freq, duty), pwm.stop(pin)
 *   import sleep   # sleep.ms(ms)
 */

/**
 * Register all hardware-binding modules into the MicroPython runtime.
 * Must be called after mp_embed_init() and before mp_embed_exec_str().
 */
void mpy_gpio_modules_register(void);
