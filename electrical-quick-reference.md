# CNC Press Brake — Electrical Quick Reference

## Controller

- **ESP32:** common 30-pin ESP32 DevKit V1 / ESP-WROOM-32 development board.
- **Board power:** regulated 5 V into `VIN`/`5V`; never feed 5 V into `3V3`.
- **Logic level:** all ESP32 I/O is 3.3 V. ESP32, TMC logic, and 24 V power supply grounds must share a common ground.
- **Network/UI:** Wi-Fi uses no external GPIO. The intended UI link is a WebSocket over the local network; motion pulses are generated locally on the ESP32.

## Motion hardware

- **Motors:** four NEMA 17 steppers.
  - Every motor has its own `STEP` signal.
  - All four drivers share one global `EN` signal.
  - The two vertical/punch motors share one `DIR` signal.
  - The two horizontal/backgauge motors share one `DIR` signal.
  - Separate STEP signals allow either motor in a pair to stop independently during homing/squaring. A shared enable does not prevent this; it only means the four driver power stages energize or disable together.
  - Lead screws: T8×8 (8 mm lead per revolution).
- **Drivers:** four BIGTREETECH TMC2209 stepstick drivers (V1.3).
  - `VM`/`VS`: 24 V motor supply (the V1.3 board is rated for 12-28 V DC).
  - `VIO`: ESP32 `3V3`.
  - `STEP`, `DIR`, `EN`, `PDN_UART`, and optional `DIAG` are logic connections.
  - `EN` is active-low.
  - BTT specifies active cooling when operating above 1.2 A.
  - Do not connect any motor-supply voltage to ESP32 GPIO.

## ESP32 pinout — selected assignment

| GPIO | Assignment | Connects to |
|---:|---|---|
| 25 | Vertical motor 1 STEP | STEP on vertical TMC 1 |
| 14 | Vertical motor 2 STEP | STEP on vertical TMC 2 |
| 26 | Vertical DIR | DIR on both vertical TMCs |
| 32 | Horizontal motor 1 STEP | STEP on horizontal TMC 1 |
| 13 | Horizontal motor 2 STEP | STEP on horizontal TMC 2 |
| 33 | Horizontal DIR | DIR on both horizontal TMCs |
| 27 | Global driver enable | EN on all four TMCs |
| 16 | TMC UART RX | Shared bus wired directly to `RX` on all four V1.3 TMCs |
| 17 | TMC UART TX | Same shared bus through ~1 kΩ; do not wire directly to a TMC `TX` pin |
| 34 | Vertical limit 1 | Left vertical mechanical limit |
| 35 | Vertical limit 2 | Right vertical mechanical limit |
| 36 / VP | Horizontal limit 1 | Backgauge home mechanical limit |
| 39 / VN | Vertical TMC 1 DIAG | Primary vertical StallGuard/error input |
| 18 | Horizontal TMC 1 DIAG | Primary horizontal StallGuard/error input |
| 19 | Vertical TMC 2 DIAG | May later become an extra limit input |
| 23 | Horizontal TMC 2 DIAG | May later become an extra limit input |
| 4 | Indicator LED | GPIO4 → 330–470 Ω → LED → GND |
| 21 | Reserved | Spare GPIO; may become vertical-axis EN, an extra limit, or I²C SDA |
| 22 | Reserved | Spare GPIO; may become horizontal-axis EN, an extra limit, or I²C SCL |

Add one external 10 kΩ pull-up from the shared TMC `EN` line to `3V3`. The drivers then remain disabled while the ESP32 is resetting or GPIO27 is floating. Firmware drives GPIO27 LOW only when the motor system is allowed to energize.

If independent enable control becomes useful later, use GPIO21 for the two vertical EN pins and GPIO22 for the two horizontal EN pins. Individual enable per motor is unnecessary for independent stepping or homing because each driver already has its own STEP signal.

## TMC UART bus

All four drivers share the UART bus but require unique addresses:

| Driver | MS2 | MS1 | UART address |
|---|---|---|---:|
| Vertical 1 | GND | GND | 0 |
| Vertical 2 | GND | 3V3/VIO | 1 |
| Horizontal 1 | 3V3/VIO | GND | 2 |
| Horizontal 2 | 3V3/VIO | 3V3/VIO | 3 |

### V1.3 UART wiring

The TMC2209 IC uses one-wire, half-duplex UART. On the BIGTREETECH V1.3 module, the side pin silk-screened `RX` is connected to the IC's `PDN_UART` signal by default. The side pin silk-screened `TX` is not connected by default because the `R10` link is open.

V1.3 physical pin labels (follow the module silk screen if the board is rotated):

| Control side | Power/motor side | Auxiliary top header |
|---|---|---|
| `EN`, `MS1`, `MS2`, `RX`, `TX`, `CLK`, `STEP`, `DIR` | `VM`/`VS`, `GND`, `A2`, `A1`, `B1`, `B2`, `VIO`, `GND` | `INDEX`, `DIAG`, `VREF` |

Wire the shared bus as follows:

```text
ESP32 GPIO17 TX -- ~1 kΩ --+
                            +-- RX on all four TMC2209 V1.3 modules
ESP32 GPIO16 RX ------------+
```

- Leave every TMC module's side `TX` pin disconnected. Do not bridge `R10` for this wiring scheme.
- Leave `CLK` disconnected; the driver uses its internal clock.
- The auxiliary pins at the top are `INDEX`, `DIAG`, and `VREF` (some supplied headers may populate only two). Only `DIAG` is used by this project. `INDEX` is unused, and `VREF` is a current-reference/test point, not a UART pin.
- V1.3 ships in UART mode; no solder modification is required for the default `RX`/`PDN_UART` connection.

UART provides configuration and status from every driver. Individual `DIAG` lines provide immediate stall/error indications. StallGuard is supplemental feedback, not a substitute for mechanical switches.

Sources: [BIGTREETECH TMC2209 V1.3 schematic](https://github.com/bigtreetech/BIGTREETECH-Stepper-Motor-Driver/blob/master/TMC2209/V1.3/Schematic/TMC2209%20V1.3-SCH.pdf) and [V1.3 user manual](https://github.com/bigtreetech/BIGTREETECH-Stepper-Motor-Driver/blob/master/TMC2209/V1.3/manual/BIGTREETECH%20TMC2209%20V1.3%20User%20Manual.pdf).

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

GPIO21 and GPIO22 remain available. They may be used as two additional direct limit-switch inputs, split into vertical and horizontal enable signals, or reserved as the default SDA/SCL pair for a future I²C expander.

For the first two additional mechanical limits, use GPIO21 and GPIO22. If more are required, next reassign GPIO19 and/or GPIO23 from the second drivers' `DIAG` signals. UART status from all four drivers and the primary vertical/horizontal DIAG signals will remain available.

For larger expansion, retain GPIO21 and GPIO22 as SDA/SCL for an MCP23017 I²C I/O expander. Keep STEP, DIR, EN, the three primary mechanical limits, and the TMC DIAG signals directly on the ESP32.
