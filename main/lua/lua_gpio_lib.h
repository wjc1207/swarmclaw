#pragma once

#include "lua.h"

/**
 * Open hardware-binding libraries into a Lua state.
 *
 * Registers the following globals:
 *   gpio.write(pin, val)       gpio.read(pin)
 *   i2c.write(sda,scl,addr,data,freq)  i2c.read(sda,scl,addr,len,freq)
 *   spi.transfer(mosi,miso,sclk,cs,tx)
 *   rgb.fill(pin,n,r,g,b)     rgb.show(pin,n)
 *   pwm.start(pin,freq,duty)
 *   sleep.ms(ms)
 */
void lua_open_gpio_libs(lua_State *L);
