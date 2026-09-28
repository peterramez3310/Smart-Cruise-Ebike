# Firmware (ESP32)

Code: [smart_cruise_ebike/smart_cruise_ebike.ino](smart_cruise_ebike/smart_cruise_ebike.ino)

The ESP32 runs the PID loop, drives the X9C103S digital potentiometer, and switches the relay.

## What the code does

1. Boots with the relay in NC (manual throttle) and homes the X9C103S wiper to 0.
2. Measures speed from the pulse signal of the controller's LCD/instrument wire.
3. On the cruise button: saves the current speed as the set-point, moves the wiper close to the current throttle level, then switches the relay to NO.
4. Every 200 ms the PID moves the wiper up or down (at most 3 steps per loop).
5. Cruise ends and manual throttle returns when: brake pressed, button pressed again, speed signal lost, speed too low, or the bike cannot hold the speed for 4 seconds.

If the ESP32 resets or loses power, the relay falls back to NC. The rider keeps the manual throttle.

## Requirements

- Arduino IDE 2.x
- ESP32 board package (Espressif)
- No extra libraries

## How to upload

1. Open `smart_cruise_ebike/smart_cruise_ebike.ino`.
2. Board: **ESP32 Dev Module** (or your board).
3. Select the port and click Upload.
4. Open Serial Monitor at **115200** to see the log.

## Pins used in the code

Change them in the CONFIG section if your wiring is different.

| Function | ESP32 pin |
|---|---|
| Speed signal input | GPIO 34 |
| Cruise button (to GND) | GPIO 27 |
| Brake cut-off signal | GPIO 26 |
| Relay control | GPIO 25 |
| X9C103S INC | GPIO 18 |
| X9C103S U/D | GPIO 19 |
| X9C103S CS | GPIO 23 |

## Before the first ride (checklist)

Test with the rear wheel lifted first.

1. **Voltage levels.** ESP32 pins accept **3.3 V only**. Measure the speed signal from the controller and the brake signal. If they are above 3.3 V, use a voltage divider or an optocoupler.
2. **Wiper direction.** Measure the X9C103S wiper (VW) with a multimeter. It should rise when the code steps up.
3. **Wiper range.** Measure your throttle voltage at idle and at full. Set `WIPER_MIN` and `WIPER_MAX` to match (each step is about 0.05 V).
4. **Relay logic.** If the relay is active-LOW, set `RELAY_ACTIVE_HIGH = false`.
5. **Brake wire.** Wire the brake cut-off signal to GPIO 26. Set `BRAKE_ACTIVE_LOW` to match your brake switch.
6. **Speed calibration.** Read `hz` in the Serial Monitor at a known speed. Then set `PULSES_PER_WHEEL_REV` so that `speed_kmh` matches the display.

## PID tuning

Gains are in wiper steps per km/h. Start with the defaults and change one value at a time.

| Symptom | Change |
|---|---|
| Speed swings up and down | Lower `KP` or `KI` |
| Slow to reach the set speed | Raise `KI` a little |
| Overshoot | Raise `KD` a little |
| Never reaches the set speed | Check `WIPER_MAX` and battery level |

Final values used in the competition: Kp = ___  Ki = ___  Kd = ___
