# CNC Press Brake — Electrical Quick Reference

## Controller

- **ESP32:** common 30-pin ESP32 DevKit V1 / ESP-WROOM-32 development board.
- **Board power:** regulated 5 V into `VIN`/`5V`; never feed 5 V into `3V3`.
- **Logic level:** all ESP32 I/O is 3.3 V. ESP32, TMC logic, and 24 V power supply grounds must share a common ground.
- **Network/UI:** Wi-Fi uses no external GPIO. The intended UI link is a WebSocket over the local network; motion pulses are generated locally on the ESP32.

## Motion hardware

- **Motors:** four NEMA 17 steppers.
  - Two vertical/punch motors share command signals.
  - Two horizontal/backgauge motors share command signals.
  - Lead screws: T8×8 (8 mm lead per revolution).
- **Drivers:** four BIGTREETECH TMC2209 stepstick drivers (V1.2).
  - `VM`: 24 V motor supply.
  - `VCC_IO`: ESP32 `3V3`.
  - `STEP`, `DIR`, `EN`, `PDN_UART`, and optional `DIAG` are logic connections.
  - `EN` is active-low.
  - Do not connect any motor-supply voltage to ESP32 GPIO.

## ESP32 pinout — selected assignment

| GPIO | Assignment | Connects to |
|---:|---|---|
| 25 | Vertical STEP | STEP on both vertical TMCs |
| 26 | Vertical DIR | DIR on both vertical TMCs |
| 32 | Horizontal STEP | STEP on both horizontal TMCs |
| 33 | Horizontal DIR | DIR on both horizontal TMCs |
| 27 | Global driver enable | EN on all four TMCs |
| 16 | TMC UART RX | Shared TMC `PDN_UART` bus |
| 17 | TMC UART TX | Shared `PDN_UART` bus through ~1 kΩ |
| 34 | Vertical limit 1 | Left vertical mechanical limit |
| 35 | Vertical limit 2 | Right vertical mechanical limit |
| 36 / VP | Horizontal limit 1 | Backgauge home mechanical limit |
| 39 / VN | Vertical TMC 1 DIAG | Primary vertical StallGuard/error input |
| 18 | Horizontal TMC 1 DIAG | Primary horizontal StallGuard/error input |
| 19 | Vertical TMC 2 DIAG | May later become an extra limit input |
| 23 | Horizontal TMC 2 DIAG | May later become an extra limit input |
| 4 | Red indicator LED | Series resistor, then LED to GND |
| 13 | Yellow indicator LED | Series resistor, then LED to GND |
| 14 | Green indicator LED | Series resistor, then LED to GND |
| 21 | Pushbutton 1 | Button to GND; firmware uses pull-up |
| 22 | Pushbutton 2 | Button to GND; firmware uses pull-up |

## TMC UART bus

All four drivers share the UART bus but require unique addresses:

| Driver | MS2 | MS1 | UART address |
|---|---|---|---:|
| Vertical 1 | GND | GND | 0 |
| Vertical 2 | GND | 3V3/VCC_IO | 1 |
| Horizontal 1 | 3V3/VCC_IO | GND | 2 |
| Horizontal 2 | 3V3/VCC_IO | 3V3/VCC_IO | 3 |

UART provides configuration and status from every driver. Individual `DIAG` lines provide immediate stall/error indications. StallGuard is supplemental feedback, not a substitute for mechanical switches.

## Limit switches

- **Switch:** HiLetgo KW12-3 roller-lever microswitch.
- Use **COM** and **NC** terminals; leave NO unused.
- Each switch uses one GPIO plus GND (two conductors); all switches share controller ground.
- Wire the GPIO with an external 4.7–10 kΩ pull-up to `3V3`:

```text
3V3 ── 4.7–10 kΩ ── GPIO ── NC switch COM ── GND
```

- Normal = LOW; pressed or broken wire = HIGH.
- GPIO34, 35, 36, and 39 have no internal pull-up, so external pull-ups are mandatory.
- Optional later change: reassign GPIO19 and/or GPIO23 from secondary TMC `DIAG` to additional limit switches. UART status from all four TMCs remains available.

## Pins to leave alone

- `GPIO0`, `GPIO2`, `GPIO5`, `GPIO12`, and `GPIO15`: boot/strapping pins; avoid for this project.
- `GPIO1/TX0` and `GPIO3/RX0`: USB programming/debugging.
- `EN`: ESP32 reset.

## Expansion note

No uncomplicated ESP32 GPIO remains after the assignments above. For more switches, buttons, or LEDs, add an MCP23017 I²C I/O expander later. GPIO21/22 can be repurposed as its SDA/SCL lines; keep primary motion, main limits, and TMC DIAG signals directly on the ESP32.
