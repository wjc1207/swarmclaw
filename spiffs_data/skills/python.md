# SKILL: Python Hardware Scripting via MicroPython

## Purpose
Use this skill whenever you need to write Python scripts that control hardware
peripherals on an ESP32 device using the built-in MicroPython hardware modules.
This skill covers every available module: `gpio`, `i2c`, `spi`, `rgb`, `pwm`,
and `time_ms`.

---

## Module Reference

### `gpio` — Digital I/O

| Function | Signature | Returns | Description |
|---|---|---|---|
| `gpio.write` | `gpio.write(pin, value)` | `None` | Set a digital output pin HIGH (1) or LOW (0) |
| `gpio.read` | `gpio.read(pin)` | `int` | Read the digital level of a pin (0 or 1) |

```python
import gpio

# Turn on an LED on pin 2, then read pin 4
gpio.write(2, 1)
level = gpio.read(4)
print("Pin 4 level:", level)
```

---

### `i2c` — I²C Bus

| Function | Signature | Returns | Description |
|---|---|---|---|
| `i2c.write` | `i2c.write(sda, scl, addr, data_list, [freq])` | `None` | Write bytes to an I²C device |
| `i2c.read` | `i2c.read(sda, scl, addr, len, [freq])` | `str` (JSON array) | Read `len` bytes from an I²C device |

**Defaults:** `freq` defaults to `100000` (100 kHz) when omitted.

`data_list` must be a Python list of integers (byte values 0–255).

```python
import i2c

# Write register 0x00 with value 0xFF to device at address 0x3C
# SDA=21, SCL=22, 400 kHz
i2c.write(21, 22, 0x3C, [0x00, 0xFF], 400000)

# Read 6 bytes from device 0x68 (e.g. MPU-6050)
raw = i2c.read(21, 22, 0x68, 6)
print("Raw bytes:", raw)
```

---

### `spi` — SPI Bus

| Function | Signature | Returns | Description |
|---|---|---|---|
| `spi.transfer` | `spi.transfer(mosi, miso, sclk, cs, tx_list)` | `str` (JSON result) | Full-duplex SPI transfer |

`tx_list` is a Python list of bytes to send. The return value contains the
received bytes from the bus.

```python
import spi

# Send 0x9F (JEDEC ID command) and read 3 response bytes
# MOSI=23, MISO=19, SCLK=18, CS=5
rx = spi.transfer(23, 19, 18, 5, [0x9F, 0x00, 0x00, 0x00])
print("SPI RX:", rx)
```

---

### `rgb` — Addressable RGB LEDs (NeoPixel / WS2812)

| Function | Signature | Returns | Description |
|---|---|---|---|
| `rgb.fill` | `rgb.fill(pin, num_pixels, r, g, b)` | `None` | Set all pixels to a single RGB colour (0–255 each) |
| `rgb.show` | `rgb.show(pin, num_pixels)` | `None` | Latch/push the pixel data to the strip |

`rgb.fill` only buffers the colour. You **must** call `rgb.show` to update the
physical LEDs.

```python
import rgb
import time_ms

# Set 8 NeoPixels on pin 16 to solid red, then display
rgb.fill(16, 8, 255, 0, 0)
rgb.show(16, 8)

# Simple colour sweep
colours = [
    (255, 0,   0),   # red
    (0,   255, 0),   # green
    (0,   0,   255), # blue
]
for r, g, b in colours:
    rgb.fill(16, 8, r, g, b)
    rgb.show(16, 8)
    time_ms.sleep(500)
```

---

### `pwm` — PWM Output

| Function | Signature | Returns | Description |
|---|---|---|---|
| `pwm.start` | `pwm.start(pin, freq, duty)` | `None` | Start PWM on a pin. `duty` is 0–1023 (10-bit) |

```python
import pwm

# 50% duty cycle at 1 kHz on pin 13
pwm.start(13, 1000, 512)

# Servo pulse: 50 Hz, duty ~77 ≈ 1.5 ms centre position (10-bit scale)
pwm.start(15, 50, 77)
```

> **Duty scale:** 0 = 0%, 1023 = 100%. Calculate with:
> `duty = int(percentage / 100 * 1023)`

---

### `time_ms` — Delays

| Function | Signature | Returns | Description |
|---|---|---|---|
| `time_ms.sleep` | `time_ms.sleep(ms)` | `None` | Block for `ms` milliseconds (uses FreeRTOS `vTaskDelay`) |

```python
import time_ms

time_ms.sleep(1000)   # wait 1 second
time_ms.sleep(50)     # wait 50 ms
```

---

## Error Handling

All library functions raise a `RuntimeError` if the underlying
`tool_gpio_execute` call fails. Wrap calls in `try/except` for recoverable error
handling:

```python
import gpio

try:
    gpio.write(99, 1)   # invalid pin
except RuntimeError as e:
    print("GPIO error:", e)
```

---

## Common Patterns

### Blink an LED
```python
import gpio
import time_ms

LED_PIN = 2
while True:
    gpio.write(LED_PIN, 1)
    time_ms.sleep(500)
    gpio.write(LED_PIN, 0)
    time_ms.sleep(500)
```

### Read a button with debounce
```python
import gpio
import time_ms

BTN_PIN = 0
last = gpio.read(BTN_PIN)
while True:
    time_ms.sleep(20)
    cur = gpio.read(BTN_PIN)
    if cur != last:
        print("Button changed to:", cur)
        last = cur
```

### I²C sensor read-register helper
```python
import i2c

def i2c_read_reg(sda, scl, addr, reg, length):
    """Write a register address, then read N bytes back."""
    i2c.write(sda, scl, addr, [reg])
    return i2c.read(sda, scl, addr, length)

# Example: read WHO_AM_I register (0x75) of MPU-6050 at 0x68
device_id = i2c_read_reg(21, 22, 0x68, 0x75, 1)
print("WHO_AM_I:", device_id)
```

### PWM LED fade
```python
import pwm
import time_ms

PIN = 13
# Fade in
for duty in range(0, 1024, 8):
    pwm.start(PIN, 1000, duty)
    time_ms.sleep(10)
# Fade out
for duty in range(1023, -1, -8):
    pwm.start(PIN, 1000, duty)
    time_ms.sleep(10)
```

### NeoPixel rainbow cycle
```python
import rgb
import time_ms
import math

PIN = 16
N = 12

def hsv_to_rgb(h):
    """Convert hue (0-255) to RGB tuple."""
    s, v = 255, 255
    i = h // 43
    f = (h - i * 43) * 6
    q = (255 - f) * v // 255
    t = f * v // 255
    if   i == 0: return (v, t, 0)
    elif i == 1: return (q, v, 0)
    elif i == 2: return (0, v, t)
    elif i == 3: return (0, q, v)
    elif i == 4: return (t, 0, v)
    else:        return (v, 0, q)

for hue in range(255):
    r, g, b = hsv_to_rgb(hue)
    rgb.fill(PIN, N, r, g, b)
    rgb.show(PIN, N)
    time_ms.sleep(20)
```

---

## Rules & Constraints

1. **Always use integer pin numbers.** Passing a float or `None` will raise an
   error.
2. **`rgb.fill` must be followed by `rgb.show`** before colours appear on the
   hardware.
3. **I²C / SPI `data` arguments must be Python lists** (not strings or bytes).
4. **`i2c.read` and `spi.transfer` return a JSON-encoded string**, not a plain
   number. Parse with `json.loads(result)` if needed.
5. **Duty cycle for PWM is 0–1023** (10-bit). Values outside this range may
   produce unexpected behaviour.
6. **`time_ms.sleep` blocks the FreeRTOS task.** Keep delay values reasonable to
   avoid watchdog timeouts (stay well under the configured WDT period).
7. **No persistent state between calls.** Each `gpio`, `i2c`, `spi`, etc. call
   is stateless; bus/pin configuration is handled inside the tool layer.

---

## Quick-Reference Card

```
import gpio, i2c, spi, rgb, pwm, time_ms

gpio.write(pin, 0|1)
gpio.read(pin)                         → 0|1

i2c.write(sda, scl, addr, [bytes], [freq])
i2c.read(sda, scl, addr, len, [freq]) → "[b0,b1,…]"

spi.transfer(mosi, miso, sclk, cs, [tx_bytes]) → "[rx_bytes]"

rgb.fill(pin, n, r, g, b)
rgb.show(pin, n)

pwm.start(pin, freq_hz, duty_0_1023)

time_ms.sleep(milliseconds)
```
