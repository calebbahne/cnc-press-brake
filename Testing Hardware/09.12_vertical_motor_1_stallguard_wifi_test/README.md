# Motor 1 StallGuard Wi-Fi test

Open `09.12_vertical_motor_1_stallguard_wifi_test.ino` in Arduino IDE. Keep
`control_page.h` in the same folder. Board: **DOIT ESP32 DEVKIT V1**; libraries:
**TMCStepper** and **WebSockets** (Markus Sattler). Serial Monitor: **115200**.
The sketch uses the same Wi-Fi credentials and hostname as the 09.07 test.
After upload, Serial prints the IP. Open that address or
http://cnc-press-brake.local from a device on the same network.

## Connect before testing

- Motor 1 STEP GPIO25, DIR GPIO26, active-low global EN GPIO27.
- TMC2209 address 0: MS1 and MS2 grounded, external 0.110-ohm sense resistors.
- UART GPIO16 directly to the driver RX/PDN_UART bus, GPIO17 through ~1 kΩ to
  the same bus. Leave the BTT V1.3 side TX pin disconnected. Supply 24 V, 3.3 V
  VIO and common grounds as in the earlier UART test.
- Driver **DIAG → GPIO39/VN**. Use an external 10 kΩ pull-down to ground for a
  defined disconnected state; GPIO39 has no internal pull resistor. A low DIAG
  input alone cannot prove that its wire is connected.
- NC limits: GPIO34 and GPIO35, each with an external pull-up to 3.3 V and a
  normally-closed switch to ground. Either HIGH blocks/stops motion. If these
  are not installed, explicitly uncheck the limit requirement **only with a
  mechanically detached bench motor**. Disabling the check is not homing.
- Use the existing external 10 kΩ EN pull-up. Other STEP outputs (14, 32, 13)
  remain LOW, but global EN enables all connected driver power stages. To test
  only this motor's supply consumption, isolate other drivers with power off.

This is a single-motor commissioning test. Do not operate one motor against a
mechanically coupled vertical pair. Support the vertical load: disabling EN on
network/UART/electrical failure removes holding torque. There is no homing,
absolute travel limit, encoder, or forming-force measurement here.

## Starter settings

| Control | Default | Adjustable range |
|---|---:|---:|
| Run current | 600 mA RMS per winding | 300–1000 mA |
| Hold current | 50% | 30–100% |
| Stationary precharge | 400 ms at run current | 100–2000 ms |
| Target speed | 250 full steps/s | 20–600 |
| Acceleration | 100 full steps/s² | 10–1000 |
| Initial speed | 20 full steps/s | 5–100, no greater than target |
| Sensing minimum | 200 full steps/s | 50–600 |
| Settling at target speed | 300 ms | 100–2000 |
| Per-jog step budget | 600 full steps | 20–2000 |
| SGTHRS | 0 | 0–255 |
| Stop on DIAG | Off for baseline capture | On/off |
| NC limits required | On | Explicit detached-bench opt-out |
| SG at 0% / 100% | 510 / 0 | Two editable endpoints, 0% endpoint must be higher |

Full stepping, interpolation OFF, CoolStep OFF and StealthChop ON are fixed.
The installed TMCStepper library uses `microsteps(0)` to select full stepping;
`microsteps(1)` would silently leave the previous setting unchanged.

With a 200-step/revolution motor and T8×8 screw, 250 full steps/s is 75 RPM or
10 mm/s. The default 600-step budget is **24 mm maximum commanded travel per
jog**, including the ramp. Release earlier for the first short motion check.
A full default jog reaches the settled sensing window; very short jogs may
never show a valid load percentage. Budget exhaustion latches a fault. It is
not an absolute travel limit: successive jogs can accumulate arbitrary travel.

The acceleration ramp takes about 2.3 seconds from 20 to 250 full steps/s.
Lower acceleration reduces torque consumed by acceleration; increasing it does
not increase available motor torque. Precharge holds IHOLD=IRUN throughout the
precharge and motion, without increasing current above the selected limit.
On stopping, the configured hold percentage is restored. This is electrical
precharge, not a mechanical preload or a guarantee of overcoming static friction.

The 1000 mA software ceiling is intentionally below the guide's active-cooling
threshold; the motor's own rating and actual temperatures may require less.
**RMS winding current is not power-supply current.** No current sensor is
installed, so software cannot guarantee supply draw below 2 A. Watch an actual
supply meter while testing, including startup and hold; a 4 A fuse does not
enforce a 2 A limit. The ceiling cannot be raised from the webpage.

## Operation and calibration

1. Confirm the earlier UART test passes, then upload this sketch. Look for
   `CONFIG PASS` and the IP in Serial. Outputs start disabled.
2. Set the NC-limit option for the actual bench wiring. Click **Apply settings /
   clear fault** while disabled. Invalid values are rejected on the ESP32.
3. Click **Arm / hold torque**, then hold UP or DOWN. Release to stop STEP pulses
   immediately and retain hold current. Stop before changing direction. Disable
   explicitly before changing settings. Space or STOP also stops motion.
4. Inspect raw SG_RESULT and DIAG rising-edge count. A pulse can be too short
   to see in the live pin state, so the interrupt counter is important.
5. After basic motion works, allow a jog long enough to reach cruise and settle.
   The percentage is blank at rest, during precharge/ramp/settling, below the
   configured sensing minimum, or with invalid telemetry. It is also marked
   stale on lost telemetry. Raw SG can retain an old value when idle.
6. Record lightly loaded and heavily loaded SG values at the intended current,
   speed and direction. Enter those as the 0% and 100% endpoints. Do not provoke
   a hard-stop collision to calibrate. The displayed value is:

   `clamp(100 * (SG_at_0_percent - SG_RESULT) / (SG_at_0_percent - SG_at_100_percent), 0, 100)`

   This is a relative index, not percentage of rated torque, force, or supply
   power. Changing current, speed, direction or mechanics requires rechecking
   the endpoints. The default 510/0 scale is uncalibrated.
7. Use the project's StallGuard procedure to select SGTHRS from the observed
   loaded SG value (hardware compares SG_RESULT ≤ 2 × SGTHRS), then enable
   **Stop on DIAG**. Higher SGTHRS makes detection more sensitive. Validate the
   actual wire and stop path. Startup and ramp DIAG edges are counted but do
   not cause stall stops; temperature/short faults are still polled throughout.
8. Click **Print applied settings to Serial** to print all settings as JSON,
   fixed driver modes, quantized current and TCOOLTHRS. Serial `p` does the same.
   Settings are RAM-only and reset on reboot. Edited but unapplied form values
   are not exported. Copy the output for later use; there is no JSON importer.

StallGuard4 needs motion in a suitable speed range and StealthChop. Full-step
resonance, very low speed and insufficient back EMF may make readings unstable.
The default 200 full steps/s sensing minimum is a starting hypothesis, not a
validated detection threshold for this particular motor.

## Stop and communication behavior

- STEP generation runs in the ESP32 high-priority `esp_timer` task every 100 µs,
  separately from browser and UART servicing. Pulses are at least 3 µs. It does
  not issue catch-up step bursts after delays. Timing still needs scope testing
  on hardware; this is not a certified real-time or safety controller.
- Only the browser that armed the motor owns motion/heartbeat commands. Other
  clients can observe, export settings or stop/disable it.
- Heartbeats arrive every 100 ms. The local timer stops and disables after
  750 ms without one. Tab hiding/blur requests disable; reconnect never arms.
- A DIAG rising interrupt latches a stop only when stop-on-DIAG is selected and
  motion is in the settled sensing window. An already-HIGH DIAG also trips.
- NC-limit, DIAG and step-budget faults stop pulses and retain hold torque.
  Watchdog, UART/configuration and electrical faults disable outputs. A subsequent
  loss of browser heartbeat also disables a held faulted motor.
- UART status is polled about every 150 ms. Overtemperature/prewarning, shorts,
  undervoltage and unexpected driver reset fault the test. Open-load flags are
  displayed without automatically faulting because they can be spurious at rest.
- Faults do not auto-resume. Disable, inspect the cause, Apply/clear, then Arm.
  The first fault is retained until explicit clearing. Commanded position is
  only a pulse count and is unreliable after a stall.
- No authentication is supplied: use a trusted local Wi-Fi network.

## Validation

Compile checked for `esp32:esp32:esp32doit-devkit-v1` using ESP32 core 3.1.1,
TMCStepper 0.7.3 and WebSockets 2.7.2. Browser-script checks cover settings
hydration, idle/live load rendering, owner-only heartbeat, jog release, blur
disable and observer lockout. Not uploaded or tested on the physical motor;
current draw, pulse timing, torque, SG calibration and DIAG wiring/stop latency
still require bench verification.

## References

- Project `tmc2209-project-guide.md` and `electrical-quick-reference.md`.
- [Analog Devices TMC2209 datasheet, revision 1.09](https://www.analog.com/media/en/technical-documentation/data-sheets/tmc2209_datasheet_rev1.09.pdf),
  current control, StallGuard4, TSTEP normalization and STEP/DIR sections.
