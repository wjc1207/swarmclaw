# BLE Temperature Humidity (BTHome v2)

Use this skill to monitor a BLE sensor by scanning advertisements, filtering by MAC, checking BTHome v2 marker, then parsing values.

Core flow:
scan all -> filter MAC -> detect BTHome -> parse

## BTHome v2 detection

- Service Data UUID is 0xFCD2.
- In advertisement bytes this appears as D2 FC (little-endian UUID bytes).
- Minimal marker used by this project: 16 D2 FC.

## Supported data objects

- 0x02: temperature (divide by 100)
- 0x03: humidity (divide by 100)
- 0x01: battery (optional in packet; currently not returned by tool output)

## Tool usage

1. Ask for the sensor MAC address.
2. Start listening:
	{"action":"connect","addr":"aa:bb:cc:dd:ee:ff"}
3. Read latest parsed measurement:
	{"action":"read"}
4. Report temperature in C and humidity in %RH.
5. Stop listening when done:
	{"action":"disconnect"}

## Notes

- Connect action means start listening, not GATT connect.
- If no data is returned, ask user to confirm sensor is broadcasting BTHome v2 and within range.
- Encrypted BTHome payloads are not decoded in current implementation.