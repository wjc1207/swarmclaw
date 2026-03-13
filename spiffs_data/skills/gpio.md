# GPIO Tool Input Schema

Use this file as the full input reference for the `gpio` tool.

## Base schema

```json
{
  "type": "object",
  "properties": {
    "action": {
      "type": "string",
      "description": "Operation to perform"
    }
  },
  "required": ["action"]
}
```

Additional fields depend on `action`.

## Actions and parameters

- gpio_set_dir: `pin` (int), `direction` ("IN"|"OUT")
- gpio_write: `pin` (int), `value` (0|1|"HIGH"|"LOW")
- gpio_read: `pin` (int)
- gpio_set_pull: `pin` (int), `pull` ("UP"|"DOWN"|"NONE")
- gpio_on_edge: `pin` (int), `edge` ("RISING"|"FALLING"|"BOTH")

- i2c_write: `sda` (int), `scl` (int), `addr` (int), `data` (int[]), `freq` (int, optional, default 100000)
- i2c_read: `sda` (int), `scl` (int), `addr` (int), `length` (int), `freq` (int, optional, default 100000)
- i2c_write_read: `sda` (int), `scl` (int), `addr` (int), `data` (int[]), `length` (int), `freq` (int, optional, default 100000)

- spi_transfer: `mosi` (int), `miso` (int), `sclk` (int), `cs` (int), `tx` (int[]), `mode` (int 0-3, optional), `speed` (int Hz, optional)
- spi_write: `mosi` (int), `sclk` (int), `cs` (int), `tx` (int[]), `mode` (int 0-3, optional), `speed` (int Hz, optional)

- rgb_set_pixel: `pin` (int), `num_pixels` (int), `index` (int), `r` (0-255), `g` (0-255), `b` (0-255), `brightness` (0.0-1.0, optional)
- rgb_fill: `pin` (int), `num_pixels` (int), `r` (0-255), `g` (0-255), `b` (0-255), `brightness` (0.0-1.0, optional)
- rgb_set_range: `pin` (int), `num_pixels` (int), `start` (int), `end` (int), `r` (0-255), `g` (0-255), `b` (0-255), `brightness` (0.0-1.0, optional)
- rgb_show: `pin` (int)
- rgb_clear: `pin` (int)

- pwm_start: `pin` (int), `freq` (int), `duty` (int 0-100)
- pwm_set_duty: `pin` (int), `duty` (int 0-100)
- pwm_set_freq: `pin` (int), `freq` (int)
- pwm_stop: `pin` (int)

- uart_write: `tx` (int, UART TX pin), `rx` (int, UART RX pin), `baud` (int), `data` (string)
- uart_read: `tx` (int, UART TX pin), `rx` (int, UART RX pin), `baud` (int), `length` (int), `timeout` (number, seconds, optional)

- onewire_scan: `pin` (int)
- onewire_read: `pin` (int), `rom` (hex string), `command` (int)

## Convenience action

- set_rgb: onboard WS2812 helper (GPIO48), parameters: `r`, `g`, `b`

## Full property dictionary

```json
{
  "action": "string",
  "pin": "integer",
  "direction": "string(IN|OUT)",
  "value": "integer|string(0|1|HIGH|LOW)",
  "pull": "string(UP|DOWN|NONE)",
  "edge": "string(RISING|FALLING|BOTH)",
  "sda": "integer",
  "scl": "integer",
  "addr": "integer",
  "data": "array<int> or string (action-dependent)",
  "length": "integer",
  "freq": "integer",
  "mosi": "integer",
  "miso": "integer",
  "sclk": "integer",
  "cs": "integer",
  "tx": "integer or array<int> (action-dependent)",
  "rx": "integer",
  "mode": "integer",
  "speed": "integer",
  "num_pixels": "integer",
  "index": "integer",
  "r": "integer",
  "g": "integer",
  "b": "integer",
  "start": "integer",
  "end": "integer",
  "brightness": "number",
  "duty": "integer",
  "baud": "integer",
  "timeout": "number",
  "rom": "string",
  "command": "integer"
}
```
