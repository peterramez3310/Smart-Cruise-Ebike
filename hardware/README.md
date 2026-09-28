# Hardware

## Drive system

| Part | Details |
|---|---|
| Hub motor | 450 W brushless DC, 48 V, 350 to 450 RPM, about 80 to 85% efficiency |
| Motor controller | Brushless DC controller, 48 V, with overcurrent protection |
| Battery | Li-ion, 48 V, 10 Ah, about 5 hours full charge |
| Battery safety | Large disconnect switch on the positive terminal |
| Throttle | Twisting throttle with 3-speed switch and cruise button |

## Cruise control system

| Part | Details |
|---|---|
| ESP32 | PID controller |
| X9C103S | Digital potentiometer, 0 to 10 kΩ, 100 steps, controlled by INC, U/D, CS |
| XL7015 | Buck converter, 48 V to 5 V |
| Relay module | 5 V, single channel |
| Digital speedometer | Gives speed feedback from the controller's LCD/instrument wire |

## Wiring

| From | To |
|---|---|
| XL7015 input | 48 V battery (+ and -) |
| XL7015 output (5 V) | ESP32 VCC/GND and relay VCC/GND |
| Speedometer VCC/GND | 48 V battery |
| Speedometer signal | Controller LCD/instrument wire and an ESP32 input pin |
| X9C103S VCC, GND | 5 V and GND from the buck converter |
| X9C103S INC, U/D, CS | ESP32 output pins |
| X9C103S VH | 5 V |
| X9C103S VL | GND |
| X9C103S VW (wiper) | Relay NO terminal |
| Relay COM | Controller throttle signal input |
| Relay NC | Manual throttle signal wire |
| Relay control pin | ESP32 output pin |
| Cruise button | One wire to an ESP32 input pin, the other to GND |

Relay states:

- **Cruise OFF:** NC is closed. The manual throttle is in control.
- **Cruise ON:** NO is closed. The X9C103S (PID) is in control.

## Safety

The brake cut-off wires (low brake and high brake) disable the motor immediately when a brake is applied.
They also tell the ESP32 to reset the PID and return the relay to NC.
This works whatever the state of the PID controller.

## Phone charging system (design)

Dynamo (12 V AC, 6 W) -> KBP206 bridge rectifier (2 A, 600 V) -> 1000 µF / 25 V capacitor -> LM2596 buck converter (5 V) -> USB-C socket (power only).

## Wiring diagram

Add your wiring diagram image here: `hardware/wiring-diagram.png`
