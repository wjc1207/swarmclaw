# BLE UUID Raw Reader (GATT)

Use this skill in connection mode: connect to a specific BLE address, discover all services and characteristics, read raw bytes for each characteristic, then disconnect.

Core flow:
connect specific addr -> read -> disconnect

## Current behavior

- Uses GATT connection (not BTHome advertisement parsing).
- Discovers all services UUIDs on the target device.
- For each service, discovers all characteristic UUIDs.
- Reads each characteristic value and returns raw bytes.
- Output focuses on raw data list (`services_raw`), including:
  - `service_uuid`
  - `characteristic_uuid`
  - `value_handle`
  - `raw_len`
  - `raw_hex`

## Tool usage

1. Connect to target address:
   {"action":"connect","addr":"aa:bb:cc:dd:ee:ff","timeout_ms":5000}
2. Read all UUID raw values:
   {"action":"read","timeout_ms":5000}
3. Disconnect when done:
   {"action":"disconnect"}

## Common UUID quick reference

### Service UUIDs (16-bit)

- `0x1800`: Generic Access
- `0x1801`: Generic Attribute
- `0x180A`: Device Information
- `0x180F`: Battery Service
- `0x181A`: Environmental Sensing
- `0x1812`: Human Interface Device

### Characteristic UUIDs (16-bit)

- `0x2A00`: Device Name
- `0x2A01`: Appearance
- `0x2A19`: Battery Level
- `0x2A24`: Model Number String
- `0x2A25`: Serial Number String
- `0x2A26`: Firmware Revision String
- `0x2A27`: Hardware Revision String
- `0x2A28`: Software Revision String
- `0x2A29`: Manufacturer Name String
- `0x2A1F`: Temperature Celsius (legacy/implementation-specific on some devices)
- `0x2A6E`: Temperature (sint16, usually 0.01 degC)
- `0x2A6F`: Humidity (uint16, usually 0.01 %RH)

## Raw data interpretation tips

- BLE characteristic value is commonly little-endian for numeric types.
- Example: `13-08` as uint16 little-endian equals `0x0813` (2067).
- For 0x2A6E temperature, vendors often use scale 0.01 degC: `2067 -> 20.67 degC`.
- For 0x2A6F humidity, vendors often use scale 0.01 %RH: `4762 -> 47.62 %RH`.

## Notes

- `connect` action requires `addr`.
- If `read` times out, check BLE range, connection stability, and whether characteristic read is permitted.
- Unknown UUIDs are expected; keep raw bytes and parse at application layer when needed.