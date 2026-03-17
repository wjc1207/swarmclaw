# BLE Temperature Humidity

Read a BLE temperature and humidity sensor that exposes the Environmental Sensing Service. Use this when the user wants live temperature or humidity from a nearby BLE device.

## Requirements

- You need the sensor's BLE MAC address.
- The sensor must expose service `0x181A` with temperature characteristic `0x2A6E` and humidity characteristic `0x2A6F`.

## How to use

1. Ask for the BLE MAC address if the user did not provide it.
2. Call `ble` with `{"action":"connect","addr":"aa:bb:cc:dd:ee:ff"}`.
3. Call `ble` with `{"action":"read"}`.
4. Report temperature in C and humidity in %RH.
5. Call `ble` with `{"action":"disconnect"}` when finished.

## Notes

- If connect fails, tell the user to move the sensor closer and confirm the MAC address.
- If read fails after connect, mention that the device may not implement the expected Environmental Sensing characteristics.