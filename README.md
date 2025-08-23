# Nyuki-Tech

### DHT Sensor Pinout

| DHT Pin | ESP32 Pin | Function |
|---------|-----------|----------|
| VCC     | 3V3       | Power    |
| GND     | GND       | Ground   |
| DATA    | GPIO15    | Data     |

## Power Management

This project uses a **TP4056 Charge Module** for battery charging and power management. The TP4056 module connects as follows:

| TP4056 Pin | Connected To | Description         |
|------------|--------------|---------------------|
| IN+        | Solar +      | Solar panel positive input |
| IN-        | Solar -      | Solar panel negative input |
| BAT+       | Battery +    | Battery positive terminal  |
| BAT-       | Battery -    | Battery negative terminal  |
| OUT+       | System VCC   | Power output positive (to ESP32, etc.) |
| OUT-       | System GND   | Power output ground        |

This setup allows the solar panel to charge the battery, while the system draws power from the battery via the TP4056 output.