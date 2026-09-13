# TMC2209 Engineering Guide for the CNC Press Brake

## Purpose

This guide explains the TMC2209 features that matter to this project and how to
use them when controlling two motors on one mechanical axis. The main goals are:

- prevent travel into mechanical stops, tooling, or other obstructions;
- stop both motors predictably when a fault or unexpected load is detected;
- monitor the relative load on each motor;
- obtain useful torque without overheating the motors or drivers;
- keep paired motors synchronized and the mechanism square; and
- make wiring, communication, and driver faults visible to firmware.

The TMC2209 is a capable stepper driver, but it is not a safety controller, a
position sensor, or a calibrated force sensor. Those distinctions should drive
the system design.

## Project electrical arrangement

The current project uses four BIGTREETECH TMC2209 V1.3 modules and an ESP32
DevKit V1. Relevant assignments are:

| Function | ESP32 pin | Notes |
|---|---:|---|
| Vertical motor 1 STEP | GPIO25 | Independent STEP permits individual homing |
| Vertical motor 2 STEP | GPIO14 | Independent STEP permits individual homing |
| Vertical pair DIR | GPIO26 | Shared direction; both motors must command the same direction |
| Horizontal motor 1 STEP | GPIO32 | Independent STEP |
| Horizontal motor 2 STEP | GPIO13 | Independent STEP |
| Horizontal pair DIR | GPIO33 | Shared direction |
| Global driver enable | GPIO27 | Active LOW; presently enables all four drivers |
| TMC UART receive | GPIO16 | Connected directly to the shared TMC RX/PDN_UART bus |
| TMC UART transmit | GPIO17 | Connected to that same bus through about 1 kOhm |
| Vertical 1 DIAG | GPIO39/VN | Input-only ESP32 pin |
| Vertical 2 DIAG | GPIO19 | Independent driver diagnostic |
| Horizontal 1 DIAG | GPIO18 | Independent driver diagnostic |
| Horizontal 2 DIAG | GPIO23 | Independent driver diagnostic |
| Vertical limits | GPIO34, GPIO35 | NC switches with external pull-ups |
| Horizontal limit | GPIO36/VP | NC switch with external pull-up |

The address straps are intended to select addresses 0, 1, 2, and 3. This allows
all four drivers to share one half-duplex UART wire while retaining separate
registers and load/status readings.

The UART bus requires 24 V motor power as well as the logic connections. VIO
sets the digital I/O level but does not power the TMC2209 core. Leave the BTT
side pin labeled `TX` disconnected with the V1.3 default R10 arrangement; both
ESP32 UART pins join the BTT pin labeled `RX` as shown in
`electrical-quick-reference.md`.

## What the TMC2209 controls

The ESP32 still owns the motion profile. It normally decides acceleration,
velocity, direction, how many steps to send, and when to stop. The TMC2209 turns
those STEP/DIR commands into regulated currents in the two motor windings.

UART adds configuration and telemetry. It allows firmware to:

- set run and hold current;
- select microstep resolution and interpolation;
- select StealthChop or SpreadCycle behavior;
- read driver status and relative motor-load information;
- configure StallGuard and route its result to DIAG;
- enable CoolStep current adaptation;
- inspect temperature, short-circuit, open-load, and current-scale flags; and
- verify accepted writes using the interface counter `IFCNT`.

UART communication does not prove that the motor moved. A successful write
only proves that the driver accepted a register command.

## UART and addressing

The TMC2209 uses single-wire, half-duplex UART on `PDN_UART`. The MCU transmits a
request, releases the bus, and listens for the driver's reply. Every driver on a
shared wire must have a unique address selected by MS1 and MS2:

| MS2 | MS1 | Address |
|---|---|---:|
| LOW | LOW | 0 |
| LOW | HIGH | 1 |
| HIGH | LOW | 2 |
| HIGH | HIGH | 3 |

At every boot, firmware should leave EN disabled, initialize UART, contact all
expected addresses, check the IC version, apply settings, and confirm that
`IFCNT` increased. Motion should remain inhibited if any required driver is
missing or reports the wrong identity.

Do not program the TMC2209's one-time-programmable memory during development.
OTP bits cannot be cleared. Apply configuration over UART at each boot instead.

## Current, torque, and temperature

### IRUN and IHOLD

`IRUN` controls winding current while the motor is considered to be running.
`IHOLD` controls current after standstill and the configured power-down delay.
Current is RMS winding current, not power-supply current. The supply current
shown by a bench supply will not equal the programmed phase current.

Motor torque generally rises with current until magnetic saturation, heating,
the supply voltage, winding inductance, or driver limits dominate. Increasing
current past the motor's useful region mostly creates heat. Higher supply
voltage mainly helps the driver build winding current quickly and preserve
torque as speed rises; it does not safely substitute for a higher current rating.

For this project:

1. Start at 600 mA RMS while unloaded or lightly loaded.
2. Confirm smooth motion, correct coil pairing, and adequate cooling.
3. Increase in small steps, such as 100 mA, only while measuring motor-case and
   driver temperatures during a representative duty cycle.
4. Never exceed the motor manufacturer's rated phase current or a thermally
   validated limit for the BTT module, whichever is lower.
5. Use forced-air cooling before approaching the module's higher-current range;
   BTT calls for active cooling above 1.2 A.
6. Treat overtemperature prewarning as a fault requiring a controlled stop or
   current reduction, not as a normal operating state.

The TMC2209 current equation depends on the sense resistors and current-scale
setting. These BTT modules use approximately 0.110 ohm sense resistors. With
UART current control, use `I_scale_analog(false)` so the digital current command
is predictable rather than also being scaled by the VREF potentiometer. The
official [TMC2209 datasheet](https://www.analog.com/media/en/technical-documentation/data-sheets/tmc2209_datasheet_rev1.09.pdf)
describes the IRUN/IHOLD equation and recommends verifying current in the actual
application.

### Hold current

Use enough hold current to resist gravity and external forces without excessive
heating. A starting point of 40-60% of run current is reasonable for testing,
but a vertical ram may require more. Determine the final value experimentally
with the mechanism safely supported. Never rely on motor holding torque to keep
a raised hazardous load from falling; use a brake, counterbalance, mechanical
support, or another appropriate load-holding device.

`IHOLDDELAY` controls how gradually current changes from IRUN to IHOLD, and
`TPOWERDOWN` controls how long the driver waits after standstill before making
that transition. A short stationary precharge before the first STEP lets
winding current settle and is helpful when starting a loaded axis.

### Practical ways to obtain more usable torque

In order of preference:

1. Confirm correct coil pairing and solid connectors. A mispaired or intermittent
   winding can vibrate while producing little useful torque.
2. Use a current setting appropriate to the motor and validate cooling under the
   real duty cycle.
3. Reduce acceleration and initial step rate. A motor that cannot accelerate its
   inertia will stall even if its low-speed holding torque is adequate.
4. Keep the 24 V supply stiff at the driver. Higher motor voltage within the
   module rating helps preserve current and torque as speed rises.
5. Use a stationary current-precharge period before moving a loaded vertical
   axis.
6. Avoid reducing current with CoolStep in a known peak-force portion of the
   cycle unless that behavior has been carefully validated.
7. Improve mechanical advantage with gearing or a lower-lead screw if speed can
   be traded for force.
8. If the required current or torque exceeds the motor/module's continuous
   thermal capability, use a larger motor and appropriately rated driver rather
   than overdriving the TMC2209.

Do not implement an unbounded firmware rule that raises IRUN whenever SG_RESULT
falls. Binding could then command maximum current and increase damage. Any
adaptive current must remain below a fixed, thermally validated ceiling.

## StealthChop and SpreadCycle

### StealthChop2

StealthChop is a voltage-mode chopper optimized for quiet, smooth, low-speed
operation. TMC2209 StallGuard4 and CoolStep are designed to operate with
StealthChop. Automatic PWM scaling and gradient tuning should normally be
enabled.

StealthChop requires time to tune after enabling and can have less predictable
torque during the first motion after startup if it has not settled. Provide a
stationary precharge and test first-step behavior under the worst expected load.

### SpreadCycle

SpreadCycle is cycle-by-cycle current control. It reacts quickly to velocity and
load changes and often performs better at medium or high speed, high dynamics,
or around motor resonance. It is noisier. TMC2209 StallGuard4 load measurement
is not available in the same useful way while operating in SpreadCycle.

The driver can switch from StealthChop at low speed to SpreadCycle at high speed
using `TPWMTHRS`. For this press-brake project, begin with StealthChop throughout
the limited test-speed range because load monitoring is a stated goal. Consider
a high-speed SpreadCycle region only after the mechanism, StallGuard operating
window, and transition behavior have been characterized.

## Microstepping and interpolation

Microstepping makes motion smoother and reduces vibration, but it does not
multiply absolute positioning accuracy or holding torque by the microstep
count. Compliance, backlash, friction, missed steps, frame deflection, and motor
load angle remain important.

A practical starting configuration is 16 external microsteps with MicroPlyer
interpolation to 256 internal microsteps. This keeps the ESP32 STEP rate
manageable while producing smooth current waveforms. MicroPlyer works best with
a low-jitter STEP signal, so motion pulses should come from a deterministic
timer/task rather than browser or network timing.

With a 200-full-step motor, 16 microsteps, and the project's T8x8 lead screw:

```text
steps per revolution = 200 * 16 = 3200
nominal travel per input step = 8 mm / 3200 = 0.0025 mm
```

That is command resolution, not guaranteed machine accuracy.

## StallGuard4 and SG_RESULT

StallGuard estimates motor load from electrical behavior while the motor is
moving in StealthChop. `SG_RESULT` ranges from 0 to 510:

- a higher value means lower motor load and more torque headroom;
- a lower value means higher motor load and a larger motor load angle; and
- a stall is signaled when `SG_RESULT <= 2 * SGTHRS`.

`SG_RESULT` is updated once per full step. It is not a percentage, force in
newtons, torque in newton-meters, or phase-current measurement. Its value changes
with motor type, current, velocity, direction, temperature, lubrication,
alignment, screw friction, and supply conditions.

Most importantly, StallGuard works best at medium motor velocities. At very low
speeds - often below roughly one revolution per second, depending on the motor -
back EMF is too small for a stable measurement. Very high speed and resonance
regions can also make it unreliable. These limitations are described in the
[datasheet's StallGuard4 section](https://www.analog.com/media/en/technical-documentation/data-sheets/tmc2209_datasheet_rev1.09.pdf#page=60).

### How this project should use StallGuard

Use StallGuard as:

- a relative per-motor load trend while moving in a calibrated speed range;
- an early warning for increasing friction, skew, binding, or tool contact;
- a secondary jam/stall detector that stops both motors; and
- a way to compare the left and right motors under equivalent conditions.

Do not use StallGuard as:

- the only travel limit;
- proof of absolute position;
- a calibrated pressing-force measurement;
- reliable contact detection at crawl speed or standstill; or
- a personnel-safety input.

Log a speed-indexed baseline for each motor rather than comparing raw left and
right values directly. Two motors can have different normal SG_RESULT values.
Useful derived signals include each motor's deviation from its own baseline,
the rate at which its result is falling, and the difference between normalized
left and right load.

### Tuning SGTHRS

Tune with the real motor, current, mechanics, direction, temperature range, and
operating speed:

1. Disable CoolStep so current stays constant.
2. Run at the intended sensing speed and record unloaded SG_RESULT.
3. Apply gradually increasing known mechanical load under controlled test
   conditions.
4. Record the lowest stable SG_RESULT immediately before an unacceptable load
   or stall.
5. Start `SGTHRS` at about half that SG_RESULT because the hardware compares
   SG_RESULT against twice SGTHRS.
6. Test across approximately 75-150% of the intended sensing velocity and at
   realistic temperatures, as recommended by the datasheet.
7. Stop both paired motors on a DIAG pulse or confirmed software threshold.

Use `TCOOLTHRS` to disable DIAG stall detection below the speed where the result
is dependable. During acceleration and deceleration, either use separate
calibrated thresholds or temporarily suppress collision decisions until the
motor enters the validated velocity window.

## DIAG output

DIAG gives the ESP32 a fast, per-driver hardware indication of a configured
stall or driver error. It avoids waiting for the next UART polling cycle. The
firmware should latch a DIAG event, stop STEP generation for both motors in that
pair, transition to a fault state, and require an explicit reset/recovery
sequence.

Do not perform substantial work inside an interrupt handler. The handler should
capture the pin/time and set a flag; the real-time motion service should perform
the stop. UART telemetry can then identify overtemperature, short, open-load,
or load-related status.

DIAG stall signaling is speed-gated and requires correct StealthChop,
`TCOOLTHRS`, `TPWMTHRS`, and `SGTHRS` configuration. Test the actual DIAG wire
and stopping path; successful UART polling does not test DIAG.

## CoolStep

CoolStep automatically varies current in response to StallGuard load. It can
reduce heat and energy consumption while preserving reserve torque. `IRUN` is
the upper current bound; CoolStep does not create torque beyond the configured
maximum. `SEMIN`, `SEMAX`, `SEUP`, `SEDN`, and `SEIMIN` determine its range and
response.

For this project, leave CoolStep disabled during initial commissioning,
StallGuard calibration, homing, and any operation where a fixed current is
important for comparing load. The datasheet warns that changing current can
cause spurious stall detection and specifically recommends disabling CoolStep
during StallGuard-based homing.

Enable it only after fixed-current operation is reliable. Tune it independently
for each driver because the two sides may have different friction and loads.
Continue treating `IRUN` as a hard thermal ceiling.

## Driver status and protection

Poll `GSTAT`, `DRV_STATUS`, and `IOIN` at a modest rate when not in a
time-critical STEP section. Useful flags include:

- `otpw`: overtemperature prewarning;
- `ot`: overtemperature shutdown;
- `s2ga`/`s2gb`: winding short to ground;
- `s2vsa`/`s2vsb`: winding short to supply;
- `ola`/`olb`: open-load indications;
- `stst`: motor standstill;
- `stealth`: current chopper mode;
- `CS_ACTUAL`: current scale currently used by CoolStep; and
- `ENN`, STEP, DIR, and IC version through IOIN.

Open-load flags are not always meaningful at standstill or very low current.
Interpret flags in their documented operating conditions and debounce warnings
when appropriate. Short or overtemperature shutdown should always latch a fault.
Do not disable the TMC2209's short-circuit protections.

## Other special features and registers

### TSTEP, TCOOLTHRS, and TPWMTHRS

`TSTEP` is the measured time between STEP events, so its numeric value is
inversely related to velocity: faster motion produces a smaller TSTEP. The
driver compares TSTEP to `TCOOLTHRS` and `TPWMTHRS` to decide when StallGuard,
CoolStep, and automatic StealthChop/SpreadCycle switching are active. Calculate
or capture these thresholds from actual motion rather than treating them as
ordinary speed values.

### IFCNT

`IFCNT` is an 8-bit counter incremented for accepted UART write datagrams. Read
it before and after configuration to prove that the driver accepted writes.
Account for wraparound. IFCNT does not prove that the mechanical output moved.

### GSTAT

`GSTAT` reports reset, driver error, and charge-pump undervoltage history. Its
status bits are cleared by writing a one to the corresponding bit. Capture fault
information before clearing it.

### VACTUAL internal pulse generator

The chip can generate a constant internal velocity through `VACTUAL`, but it
does not provide acceleration ramping. Do not use it for the paired press-brake
axis. ESP32-generated STEP/DIR motion gives the central planner deterministic
acceleration, synchronized starts, soft limits, and individual per-side pulse
gating.

### INDEX

INDEX can signal a configured microstep-table position. It is useful for
instrumentation but is not an encoder and does not prove shaft or ram position.
The current project does not need it.

### Freewheel and passive braking

StealthChop supports freewheel and passive-braking choices when IHOLD is zero.
These can reduce idle heating on an unloaded horizontal axis. Do not use a
freewheel state on a gravity-loaded vertical axis unless an independent brake or
mechanical support safely holds the load.

### VREF and internal current sensing

VREF supports analog current scaling, and the IC also offers an optional
internal-sense mode. The BTT V1.3 modules already contain approximately 0.110
ohm external sense resistors. Use those external resistors and digital UART
current control for this project; leave internal sensing disabled.

### OTP memory

OTP stores selected power-up defaults. It is one-time programmable: bits can be
set but not cleared. There is little benefit here because the ESP32 can apply
and verify all required settings at every boot. Avoid OTP programming in test
sketches.

## Preventing collisions and hard-stop impacts

Use several independent layers:

1. **Mechanical and personnel protection** - guards, appropriate interlocks,
   hardwired E-stop/energy removal, load support, and physical end stops.
2. **Normally-closed hard limit switches** - wired so a broken wire looks like
   a fault. A limit event stops both motors on the affected axis.
3. **Homed position and soft limits** - no production motion until a valid home
   has been established. Refuse commands beyond the configured work envelope.
4. **Approach zones** - reduce speed and acceleration before expected limits,
   tooling contact, or the commanded endpoint. The stopping distance must fit
   inside the remaining clearance at the current speed and load.
5. **Encoder disagreement** - stop both motors if measured left/right position
   error exceeds a small tested tolerance.
6. **StallGuard trend and DIAG** - secondary detection for unexpected binding or
   contact inside its calibrated velocity window.
7. **Command watchdogs** - reject stale, malformed, out-of-sequence, or
   implausible motion commands. Network loss must never cause continued motion.

Stopping distance includes detection latency, firmware response, pulse shutdown,
mechanical inertia, screw compliance, frame deflection, and any gravitational
motion after torque is removed. Test it at the worst allowed speed and load.

## Measuring actual load

StallGuard is useful for relative motor loading, but actual forming force needs
real force sensors. Recommended options are:

- two load cells, one near each side of the ram or bed, for total force and
  left/right load balance;
- a suitably rated pressure sensor if the final actuator system is hydraulic;
- strain gauges on a designed structural member; or
- a torque transducer where the mechanical arrangement permits it.

The sensors, amplifier, mounting, overload protection, calibration, and data
acquisition must all be rated for the expected load. Use the sum of the two
force sensors for total forming force and their difference for load imbalance.
Do not infer bending force from motor supply current alone.

## Synchronizing two motors

Equal STEP counts are necessary but not sufficient. An open-loop motor can miss
steps while the controller continues counting, leaving the ram skewed.

### Minimum viable approach

- Give each motor its own STEP output.
- Keep a single motion planner for the pair so normal pulses are issued at the
  same instants.
- Home each side independently against its own NC switch: stop STEP pulses to
  the side that reaches home while the other side continues slowly.
- Back off both switches, approach again more slowly, and establish a squared
  zero.
- During normal motion, stop both motors if either limit or either DIAG trips.
- Re-home after any suspected stall, reset, power loss, or driver fault.

### Recommended controlled approach

Install an independent position sensor on each side. Linear scales measure the
actual ram position and naturally include screw backlash/compliance; motor-shaft
encoders are easier to mount but cannot detect a loose coupling or downstream
mechanical error.

Maintain:

```text
square_error = left_position - right_position
```

Use a small deadband and tightly bounded correction. At low speed, pulse only
the lagging motor until the error returns toward zero. If the error exceeds the
tested correction envelope, stop both motors rather than attempting aggressive
recovery. The existing shared DIR signal permits same-direction correction by
withholding STEP pulses from one side, but independent DIR signals would make
future recovery and diagnostics more flexible.

The current global EN line is acceptable for fail-safe pair shutdown, but it
energizes all four drivers together. Splitting vertical and horizontal enables
onto the reserved GPIO21 and GPIO22 would reduce unnecessary heating and make
axis faults easier to contain. It still makes sense to stop both motors in a
mechanically coupled pair together.

## Recommended operating state machine

### BOOT_SAFE

- Drive global EN inactive.
- Force STEP low.
- Verify limits are electrically healthy.
- Contact every expected UART address and verify the IC version.
- Apply current/chopper/microstep configuration and confirm `IFCNT` increased.
- Refuse motion after any failed prerequisite.

### HOME

- Use fixed conservative current; CoolStep off.
- Move slowly enough to stop repeatably but fast enough only if StallGuard is
  being evaluated in its valid speed range.
- Use mechanical switches as the primary home reference.
- Square the two sides independently, back off, and re-approach.

### READY

- Position is known and inside soft limits.
- Drivers may use reduced hold current.
- A new motion command must include an allowed target, speed, and acceleration.

### MOVE

- Generate deterministic acceleration-limited pulses locally.
- Monitor hard limits and encoder disagreement continuously.
- Sample UART telemetry at a rate that cannot disturb pulse generation.
- Evaluate StallGuard only in calibrated speed/current regions.

### APPROACH

- Reduce speed and acceleration before a soft limit or expected contact region.
- Switch to the appropriate force/contact sensing strategy.
- Do not expect low-speed StallGuard to provide calibrated contact detection.

### HOLD/WORK

- Stop STEP pulses at the commanded position.
- Maintain the validated hold current or use a mechanical load-holding device.
- Measure actual force with load cells if force matters to the operation.

### FAULT

- Stop pulses to both motors on the affected pair immediately.
- Remove drive energy when required by the fault analysis.
- Record the first fault cause, positions, SG_RESULT values, status registers,
  command, speed, and time.
- Do not resume automatically. Inspect, clear the cause, and re-home.

## Suggested initial UART configuration

These are commissioning values, not final production limits:

| Setting | Initial value | Reason |
|---|---:|---|
| Address | 0-3 unique | One shared bus, independent telemetry |
| `pdn_disable` | true | Use UART rather than PDN standstill behavior |
| `mstep_reg_select` | true | Select microsteps through registers |
| `I_scale_analog` | false | Predictable digital current control |
| Run current | 600 mA RMS | Conservative starting point |
| Hold current | 50% of run | Reduce idle heating; validate against gravity |
| Microsteps | 16 | Manageable STEP rate and smooth motion |
| `intpol` | true | Interpolate internally to 256 microsteps |
| `toff` | 4 | Nonzero chopper operation |
| Blank time | 24 clocks | Conservative library/example value |
| Chopper | StealthChop | Required for useful StallGuard4/CoolStep testing |
| `pwm_autoscale` | true | Automatic StealthChop amplitude regulation |
| `pwm_autograd` | true | Automatic gradient tuning |
| CoolStep | off | Calibrate fixed-current behavior first |
| SGTHRS | 0 initially | Prevent premature stall trips during baseline logging |

Never copy a final current or SGTHRS value from another machine. Both must be
validated on this exact motor/mechanism at the intended speed and temperature.

## Commissioning roadmap

Proceed one controlled layer at a time:

1. UART read test, one driver, outputs disabled - completed for vertical motor 1.
2. UART configuration plus `IFCNT` confirmation, outputs disabled.
3. Controlled enable/hold test at conservative current, no STEP pulses.
4. Slow unloaded motion with fixed current and hard-limit monitoring.
5. Repeat for the second motor independently.
6. Home and square the pair using two mechanical switches.
7. Run paired unloaded motion and verify position repeatability.
8. Log per-motor SG_RESULT versus speed, direction, current, and known load.
9. Tune SGTHRS and validate DIAG stopping over the complete intended sensing
   range.
10. Add independent position feedback and enforce a squareness limit.
11. Add calibrated force sensors if forming force or load balance is required.
12. Consider CoolStep only after fixed-current motion and fault detection are
   fully characterized.
13. Validate every fault by deliberately disconnecting one signal at a time in
   a safe, unloaded setup: UART, limit, DIAG, encoder, and network command link.

## Data worth logging

For each motion and fault, record:

- commanded and measured position for each side;
- commanded velocity and acceleration;
- direction and motion state;
- run/hold current commands and `CS_ACTUAL`;
- SG_RESULT for each motor;
- normalized SG_RESULT relative to that motor's baseline;
- DIAG and limit states;
- `GSTAT`, `DRV_STATUS`, and UART connection health;
- motor/driver temperature sensors if fitted;
- actual left/right force and total force if load cells are fitted; and
- first-fault cause and timestamp.

Recording the first cause is important. Later effects - such as UART errors
after power removal - should not overwrite the event that initiated the stop.

## Primary references

- Analog Devices/Trinamic,
  [TMC2209 datasheet, revision 1.09](https://www.analog.com/media/en/technical-documentation/data-sheets/tmc2209_datasheet_rev1.09.pdf)
- BIGTREETECH,
  [TMC2209 module documentation](https://github.com/bigtreetech/docs/blob/master/docs/TMC2209.md)
- BIGTREETECH,
  [TMC2209 V1.3 schematic](https://github.com/bigtreetech/BIGTREETECH-Stepper-Motor-Driver/blob/master/TMC2209/V1.3/Schematic/TMC2209%20V1.3-SCH.pdf)
- OSHA,
  [Guidelines for Point of Operation Guarding of Power Press Brakes](https://www.osha.gov/enforcement/directives/cpl-02-01-025)
- OSHA,
  [29 CFR 1910.212 - General Requirements for All Machines](https://www.osha.gov/laws-regs/regulations/standardnumber/1910/1910.212)
