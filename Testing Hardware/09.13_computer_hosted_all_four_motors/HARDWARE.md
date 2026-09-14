# Four-motor stage test

Open **09.13_first_test_all_four_motors.ino** in Arduino IDE. The sketch now
matches its folder name. Keep control_page.h beside it.
Board: DOIT ESP32 DEVKIT V1. Libraries: TMCStepper and WebSockets (Markus Sattler).
Existing Wi-Fi credentials are retained. Serial: 115200 baud. After upload,
open the IP printed by Serial or http://cnc-press-brake.local.

## Select installed drivers first

At the top of the sketch, set VERTICAL_1_CONNECTED, VERTICAL_2_CONNECTED,
HORIZONTAL_1_CONNECTED, and HORIZONTAL_2_CONNECTED to match the drivers
actually plugged in. Defaults are all true. False drivers are not queried,
configured, or required to pass UART checks. A stage with no selected drivers
cannot move. Selecting only one permits independent bench testing;
do not drive one side of a mechanically coupled vertical stage.

These switches describe physical installation, not electrical isolation.
All drivers share EN; horizontal drivers also share STEP. A driver marked
false but left connected can still energize and, on horizontal, receive pulses.
Change connections only with power off. Optional INVERT_V1, INVERT_V2,
INVERT_H1, and INVERT_H2 invert individual shafts over UART for mirrored
installations; verify mechanical direction before coupling the motors.
`BROWSER_WATCHDOG_MS`, in the same top section, sets how many milliseconds the
controller will wait for a browser heartbeat while armed. Its default is 750 ms;
increase it if brief network or browser delays cause watchdog faults.

| Driver | UART address | STEP | DIR | DIAG |
|---|---:|---:|---:|---:|
| Vertical 1 | 0 | 25 | 26 | 39 |
| Vertical 2 | 2 | 14 | 26 | 19 |
| Horizontal 1 | 1 | 32 | 33 | 18 |
| Horizontal 2 | 3 | 32 | 33 | 23 |

GPIO27 is shared active-low EN. UART RX16 connects directly to the shared
BTT V1.3 RX/PDN_UART bus; TX17 joins through about 1 kΩ. Leave module TX pins
disconnected. Use unique MS1/MS2 addresses from electrical-quick-reference.md,
common grounds, 3.3 V logic, and the external 10 kΩ EN pull-up.
GPIO13 remains reserved.

No limit pins are read or configured. No automatic homing, squaring,
travel endpoints, or force control is implemented. DIAG stopping defaults off.
For DIAG use, connect the selected drivers' pins above; GPIO39 needs an
external pull-down for a defined disconnected state.

## Controls

1. Check all selected drivers show UART SEEN / Config PASS. A selected driver
   that is missing blocks motion until corrected or deselected in code.
2. Edit bottom settings while outputs are disabled. Apply configures every
   selected driver and clears faults only after all required checks pass.
3. Arm to enable holding torque. Hold + or − on a stage to jog. Release, STOP,
   Escape, or Space outside an input stops pulses immediately and retains hold.
   Jog is capped by the per-command step budget and ramps down near that cap.
4. Enter a positive increment, then click Move + increment or Move − increment.
   This is a one-click finite move; the button need not be held.
5. While armed and stationary, set vertical and horizontal zero individually.
   Each resets only that stage's commanded position.
6. Enter a signed absolute position and click Go to position, or Return to zero.
   That stage must have a valid zero. Relative jogs/increments do not require zero.
   The requested distance must fit the per-command budget; oversized moves are
   rejected, never truncated or automatically split.
7. Stop before switching stages or reversing. Firmware rejects another movement
   while one is active; there is no queue or simultaneous-axis movement.

Both selected vertical motors receive every vertical pulse. Horizontal motors
receive the same physical STEP signal. Finite moves ramp up and down using
the stage's acceleration; normal completion is not a fault. STOP/release and
fault stops are immediate, without a deceleration ramp.

Positions count commanded full steps, not measured movement. Setting zero
does not find a physical reference. Reboot, disable, and faults invalidate
both zero references. Numeric counts remain visible after disable/fault for
diagnostics, but absolute moves require re-zeroing. Undetected missed steps
or mechanical slip cannot be recognized. Check position physically.

## Settings

| Setting | Default | Range |
|---|---:|---:|
| Run current per motor | 600 mA RMS | 300–1000 |
| Hold current | 50% | 30–100 |
| Stationary run-current precharge | 400 ms | 100–2000 |
| Vertical max speed | 250 full steps/s | 20–600 |
| Vertical acceleration/deceleration | 100 full steps/s² | 10–1000 |
| Horizontal max speed | 250 full steps/s | 20–600 |
| Horizontal acceleration/deceleration | 100 full steps/s² | 10–1000 |
| Shared starting/final speed | 20 full steps/s | 5–100; no greater than either max |
| Maximum steps per command/jog | 600 | 1–100000 |
| SGTHRS | 0 | 0–255 |
| DIAG sensing minimum | 200 full steps/s | 50–600 |
| Settling at maximum speed | 300 ms | 100–2000 |
| Stop on DIAG | Off | On/off |

Settings apply to all selected drivers except separate stage speed/accel.
The moving stage uses run current during stationary precharge and motion;
the other stage stays at hold current. On completion, loop servicing restores
the hold percentage. No automatic current boost, OTP writes, or nonvolatile
settings storage. Serial p or Print applied settings exports the RAM
configuration and selected/seen/configured flags.

Full-step mode uses microsteps(0) in TMCStepper. Interpolation and CoolStep
remain off, StealthChop on. At 200 full steps/revolution with T8×8 screws,
one full step is 0.04 mm; 100 steps is 4 mm. Confirm your motor specification.
The budget is per command, not an absolute travel limit; repeated commands
can accumulate travel. No limit switches are installed.

## UART, temperature and stops

Selected drivers are polled round-robin, one per 100 ms interval (roughly
400 ms per driver with all four selected, plus UART execution time).
Unselected drivers show “skipped”, not a false “missing” fault. Status includes
UART seen, configuration success, sample age, raw DRV_STATUS/GSTAT, SG_RESULT,
DIAG level and rising edges. Open-load indications do not prove a motor is
disconnected and can be spurious at rest.

TMC2209 does not report exact temperature. DRV_STATUS bits 8–11 report chip
thresholds at 120/143/150/157 °C. The page shows a coarse threshold band, each
flag, and overtemperature prewarning/shutdown separately. This is not motor
winding temperature or a precise thermometer. Missing/stale UART samples
hide temperature data. See the
[Analog Devices TMC2209 datasheet, DRV_STATUS](https://www.analog.com/media/en/technical-documentation/data-sheets/tmc2209_datasheet_rev1.09.pdf).

Overtemperature/prewarning, shorts, undervoltage, and missing selected UART
drivers stop and disable all outputs. Unexpected reset/configuration loss also
blocks motion. DIAG stops, when enabled and in the settled active-stage sensing
window, retain hold torque and invalidate zeros. Faults never auto-resume.
Disable, fix the cause, Apply / clear, then Arm and re-zero.

The browser that arms owns motion. Other viewers can STOP/disable. The local
timer disables outputs after `BROWSER_WATCHDOG_MS` without an owner heartbeat; hiding or
leaving the owner tab requests disable. Browser/Wi-Fi disconnection also stops.
Support the vertical load because disabling removes holding torque. There is
no authentication; use a trusted local network.

The timer runs every 100 µs, pulses are at least 3 µs, and late callbacks do
not produce catch-up bursts. Vertical edges are sequential GPIO writes within
the same callback, with the same pulse count. Scope/hardware verification is
still needed. Current settings are winding current, not a supply-current cap;
all four energized motors contribute supply load with only one stage moving.

## Validation

No Arduino compilation or upload was attempted, as requested. Source review
and browser JavaScript checks are performed separately. Arduino IDE compilation,
pulse timing, direction, paired mechanics, actual current, temperature, and
physical stop behavior remain for bench verification.
