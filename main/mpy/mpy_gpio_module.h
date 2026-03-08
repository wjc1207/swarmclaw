#pragma once

/**
 * MicroPython hardware-binding modules.
 *
 * Registered via MP_REGISTER_MODULE() so every agent-written script can:
 *   import gpio    # gpio.write(pin, val), gpio.read(pin), gpio.set_dir(pin, "in"/"out")
 *   import i2c     # i2c.write(sda, scl, addr, data, freq), i2c.read(...)
 *   import spi     # spi.transfer(mosi, miso, sclk, cs, tx)
 *   import rgb     # rgb.fill(pin, n, r, g, b), rgb.show(pin, n)
 *   import pwm     # pwm.start(pin, freq, duty), pwm.stop(pin)
 *   import sleep   # sleep.ms(ms)
 */
