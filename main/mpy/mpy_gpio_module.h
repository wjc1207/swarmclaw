#pragma once

#include "py/obj.h"

/**
 * Register hardware-binding modules into the current MicroPython runtime.
 *
 * Creates the following built-in modules:
 *   gpio.write(pin, val)       gpio.read(pin)
 *   i2c.write(sda,scl,addr,data,freq)  i2c.read(sda,scl,addr,len,freq)
 *   spi.transfer(mosi,miso,sclk,cs,tx)
 *   rgb.fill(pin,n,r,g,b)     rgb.show(pin,n)
 *   pwm.start(pin,freq,duty)
 *   time_ms.sleep(ms)
 */
void mpy_register_hw_modules(void);
