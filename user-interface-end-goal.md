# CNC Press Brake — UI End Goal and Langmuir Reference

Research date: September 20, 2026.

## Current implementation and updated decisions — September 20

The first implementation checkpoint is in [Press Brake](Press%20Brake/README.md). Read that README for startup, actual supported features, and physical validation still required. The earlier inventories below remain the design reference, not a statement of what is implemented.

- The user currently has **no limit switches**. Default to manually declaring the retracted Y/X positions home. Switch homing is optional and can be enabled when the planned NC switches are installed. No encoders will be added: position is always counted motor movement from the established home.
- The user reports that the vertical load will not fall by itself. Keep Stop versus Disable behavior explicit; disabling still invalidates the coordinate references.
- DIAG wires are currently absent. DIAG stopping is an optional toggle, off by default, and does not gate ordinary operation. UART driver protection remains active.
- UI and relay live on the computer. WebSocket motion communication goes to ESP32. Eventual cloud hosting/sign-in is a future architecture phase, not an authorization to expose the current unauthenticated ESP API publicly.
- Wi-Fi preference: existing preferred network, then `iPhone (8)`, then a provisioned custom network. If unavailable, use an ESP-hosted local setup page to enter credentials. Passwords are in an ignored local firmware header, not this document or browser library.
- Primary demo material is **0.2 mm aluminum flashing**. Punch/die part numbers, usable lengths, heights/depths, widths, included tool angles, and die opening are entered in the UI. Initial bend depths are manually calibrated; desired angle/width do not automatically produce a reliable new depth.
- **Y = 0 at the upper punch limit and is negative downward.** Positive X moves the backgauge toward the die. Verify physical direction before homing. Tool maximum depth is a manually measured cap relative to the same repeatable home used during calibration.
- Microstepping is selectable from Settings. Steps/mm is **25 × the selected microstep value**; the default 8× setting is 200 steps/mm. Applying it clears homes and may require corresponding pulse-setting changes.
- Checkpoint work was explicitly authorized, including new firmware in a separate folder. Arduino compilation/upload is left to the user; neither compilation nor physical motion was attempted here.
- Working first scope: six-page UI, manual homes, bounded manual movement, a single saved calibrated bend with deliberately held phases, optional switch homing implementation, settings/diagnostics, Wi-Fi fallback implementation, and simulator. Multi-bend programs, automatic angle calculation, automatic phase transitions and cloud access remain later work.

## Purpose and how to use this record

Build a computer-operated interface for tabletop stepper-driven press brake, organized like Langmuir Titan 25T BendControl. Required pages, in order: **Run, Create Bend, Punch/Die & Materials, Back Gauge, Manual Mode, Settings**. Preserve the staged interaction: position the punch, clamp the workpiece, perform the bend, retract, then continue or repeat.

This is a functionality specification for future development, not a claim that these features already exist. Page inventories below come from the six supplied images. The short operating-sequence reference comes from the official manual. Sections marked **Project adaptation** describe our intended implementation, not Langmuir behavior. Exact keyboard bindings below are proposals; the user has specified two widely separated keys for approach and Shift plus another key for pedal-style control.

Reference files:

- [Run screenshot](Langmuir%20User%20Interface%20Research/Run%20Page.webp)
- [Create Bend screenshot](Langmuir%20User%20Interface%20Research/Create%20Bend%20Tab.webp)
- [Punch/Die & Materials screenshot](Langmuir%20User%20Interface%20Research/Punch%20%26%20Die%20%26%20Materials%20Page.webp)
- [Back Gauge photo](Langmuir%20User%20Interface%20Research/Back%20Gauge%20Page%20Photo.jpg)
- [Manual Mode screenshot](Langmuir%20User%20Interface%20Research/Manual%20Mode%20Tab.webp)
- [Settings photo](Langmuir%20User%20Interface%20Research/Settings%20Page%20Photo.jpg)
- [Official Titan manual](https://www.langmuirsystems.com/pages/titan25t-assembly), especially sections IX and XIV–XVIII.
- [Electrical quick reference](electrical-quick-reference.md)
- [Existing computer-hosted backend](Testing%20Hardware/09.13_computer_hosted_all_four_motors/README.md), [hardware/control notes](Testing%20Hardware/09.13_computer_hosted_all_four_motors/HARDWARE.md), and actual source in that directory.

## Overall layout and persistent right panel

Use horizontal page tabs across the main workspace. Put the selected page on the left and a persistent **Live Report / Machine Controls** panel on the right. The full Run, Back Gauge, and Settings images show that arrangement; the other supplied images show only tab contents, so they do not establish whether the panel is visible there. Keeping it present on all six pages is our explicit design target.

The reference also has a top header with software version, restart, machine/backgauge connection indicators, and maintenance/update controls. Our header should show PC-to-controller connection, firmware version when available, and armed/fault status. Hydraulic servicing is irrelevant: Titan is hydraulic, while this project uses steppers.

### Reference right-panel inventory

| Area | Visible information or controls |
|---|---|
| Machine status | Prominent current state such as IDLE. |
| Ram position | Large current-position number with units; home indication. |
| Ram detail | Y1, Y2, and Y1/Y2 difference. |
| Load | Separate left/right ram load bars and total load, in tons. |
| Motion | Live velocity; commanded position and speed; distance remaining. |
| Inputs | Left and right safety-jog indicators, foot-pedal indicator, anti-bind indicator. |
| Speeds | Bend speed and jog speed with a settings control. |
| Actions | Hold-to-retract, Home Machine, Cancel/Stop, Kill Hydraulic Power. |

### Project adaptation

- Show commanded vertical position Y and backgauge position X, units, active stage, speed, remaining distance, zero validity, and controller fault/message. Make **commanded position** explicit; there are no position encoders in the reviewed setup.
- Show the two approach-key states and the Shift/action-key state where the reference shows palm/pedal indicators.
- Replace hydraulic load displays with truthful driver health: UART/configuration status, temperature warning flags, and optional StallGuard diagnostics. **StallGuard and motor current are not calibrated bend-force or tonnage measurements.** Show force as unavailable unless appropriate sensing is added.
- Do not show an apparent measured Y1/Y2 alignment error from identical commanded step counts. The current status exposes stage positions, not independently measured ram ends.
- Keep **Stop motion**, **Disable drivers**, and **Arm** distinct. Existing Stop preserves hold current; Disable removes holding torque globally. Neither a keyboard shortcut nor an on-screen button is an independent hardware emergency stop.
- Keep Home visibly unavailable until real homing exists. **Set zero here** and **Return to zero** are separate, honest commands.
- Show unsupported or stale inputs as unavailable, not green/healthy. Use brief information tooltips beside settings whose meaning is not obvious.

## 1. Run page

Purpose: assemble saved bends into a job and execute it with a clear selected bend and current phase.

### Visible reference contents

- Program selector and name-edit pencil; New, Save, Duplicate.
- Numbered bend cards with drag handles, selection highlight, edit pencil, and delete button.
- Each card shows bend name, desired angle and compensation, selected punch, die, material and thickness, backgauge reference/coordinates/speed, and calculated bend tonnage.
- Selected bend's Start Position, Clamp Position, and Final Bend Position below the list.
- Add Bend; large Run Program button.
- Auto Clamp-to-Bend toggle with displayed dwell time.
- Bend-mode selector.

### Project adaptation

- Support reusable bend records and saved ordered programs. Reordering must change execution order; editing a loaded bend must have explicit save behavior.
- Show the active bend, next action, and a phase strip: **Backgauge → Approach → Clamp → Bump-off → Bend → Retract → Complete**. Optional phases can be skipped visibly.
- Run should prepare the sequence and display the next required input. Selecting a program or switching tabs must not initiate motion.
- Keep Save/Duplicate, selected-bend editing, and actual persistent storage functional. Use export/import for backups; controller motion settings and saved job data are different things.
- Start with a single-bend workflow; retain the full mode selector as the eventual target. Unimplemented modes must be disabled and labeled.
- Display known positions and validated targets. A desired angle alone is insufficient to command a reliable depth without a validated geometry/calibration model.

## 2. Create Bend page

Purpose: define one reusable bend and review all resulting positions before loading it into a program.

### Visible reference contents

| Group | Fields and actions |
|---|---|
| Bend identity | Saved bend selector, name-edit pencil. |
| Setup | Punch selector, die selector, material selector. |
| Bend inputs | Desired included angle; compensation value with UNDERBENT/OVERBENT selection; material thickness; bend width. |
| Clearance | Punch-to-material starting clearance; additional retract after bend. |
| Options | Add Backgauge Move; Override Final Bend Position. |
| Backgauge move | Reference edge stop, X position, R position, jog speed. |
| Position summary | Start safety clearance, clamp, final bend, retract-after-bend positions; total stroke. |
| Estimates | Calculated bend tonnage and predicted inside bend radius. |
| Help and notes | Open Vertical Position Diagram; X/R zero-origin notes; general bend notes. |
| Record actions | New Bend Move, Save, Duplicate, Reset, Load Bend into Program. |

### Project adaptation and interpretation

- Preserve all useful inputs and the position summary. Define the coordinate origin, positive direction, and units prominently; do not reuse Titan's numeric coordinates as machine settings.
- Clearance is space for loading material; retract allowance is extra opening after the bend. Bend width describes the extent of the bend along the tooling, distinct from sheet thickness and backgauge offset.
- Compensation is a correction field, distinct from the requested finished angle and the material's saved springback assumption. Document the sign convention when implementing calculations.
- Final-position override is the practical path for empirically calibrated bends. Mark these as **manual depth** and show both requested and step-quantized positions.
- For the initial implementation, allow explicitly taught/entered approach, clamp, bend, and retract positions. Do not invent force feedback, contact detection, or an angle-to-depth formula.
- The existing machine has no motorized R axis. Retain X; omit/disable R motion and optionally record a manually set finger height as a note.
- Predicted tonnage/radius are later calculation features, not live measurements. Mark unavailable until the calculation model is implemented and checked.
- Save tooling/material references, dimensions, compensation, target positions, optional X move, speeds, and notes together. Changes that invalidate a prepared move require preparing it again.

## 3. Punch/Die & Materials page

Purpose: maintain reusable setup data in three side-by-side libraries.

| Library | Visible fields | Visible actions |
|---|---|---|
| Punch | Selected punch/name; punch height. | Selector, edit-name pencil, New, Save, Duplicate. |
| Die | Selected die/name; die height; V-die opening. | Selector, edit-name pencil, New, Save, Duplicate. |
| Materials | Selected material/name; tensile strength; inside-bend-radius percentage; material springback angle. | Selector, edit-name pencil, New, Save, Duplicate. |

**Project adaptation:** preserve the three-column organization and basic record management. Tool heights/opening provide geometry inputs; material properties support future estimates. The radius percentage is a model input whose formula/reference must be defined before use. Populate records with this machine's actual tooling and material data, not example Titan values. Library selections should flow into Create Bend; saved jobs should retain sufficient setup information to detect later library changes.

## 4. Back Gauge page

Purpose: position the material stop independently and manage its reference coordinates.

### Visible reference contents

- Home button; reference-edge selector and edit pencil; Machine coordinate-view button.
- Large X and R readouts, each with a Zero control.
- Direction pad: X−/X+ and R−/R+.
- Jog-step entry and presets: 0.0005, 0.001, 0.01, 0.1 inches; Continuous mode.
- Jog-speed readout and presets: 1, 10, 25, 50, 75, Rapid.
- Move To X/R inputs and Run.
- Stop Back Gauge; status; four input indicators labeled X1, X2, R1, R2.

### Project adaptation

- Expose **one X stage driven by two horizontal motors**, not two independently addressable gauge axes. Their STEP wire is shared.
- Provide held X+/X− jogs, step increment, speed, set-reference/zero, absolute target, and stop. Keep physical homing separate from coordinate zeroing.
- Support named finger/reference offsets eventually; for the first version, one clearly named reference plus setup notes is adequate.
- Do not copy inch presets below the machine's positioning resolution. With the documented 200-step motor and 8 mm lead, one full step is 0.04 mm; validate this calibration on the hardware. Display quantization rather than silently promising finer motion.
- Rapid means a validated configured speed, not an unrestricted maximum.
- Planned switch wiring belongs in the electrical reference; it is not evidence that firmware currently enforces limits. Show only inputs actually reported.

## 5. Manual Mode page

Purpose: directly position the ram, teach positions, and run a simple start/end stroke independently of a saved bend program.

### Visible reference contents

- Temporary maximum tonnage per ram.
- Manual Stroke Operation: safety-plane clearance, start ram position, ending ram position, derived stroke distance, Repeat, Run Manual Stroke Operation.
- Manual Jog Step: custom increment, 0.0005/0.001/0.01/0.1-inch presets, +Y Ram Up, −Y Ram Down, Repeat.
- Move To: Y-axis machine-coordinate target and Run.
- Manual Continuous Jog entry/control.
- Manual-mode state indicator.

### Project adaptation

- Keep step jog, held continuous jog, absolute positioning, and start/end stroke as distinct operations.
- Show the direction mapping explicitly after verifying physical motion. Both vertical motors normally move together.
- Add **Use current position** controls to teach start, clamp, final, and retract positions for Create Bend.
- A temporary motor-current setting is not a tonnage limit. Keep current tuning in Advanced Settings, with actual units; omit unsupported force control.
- Repeat must require continued deliberate input and a visible repeat state; never turn a single keypress into an unattended repeating stroke.
- Manual-mode changes must not leave a program running in the background. Retain stop, connection freshness, ownership, and zero-validity checks.

## 6. Settings page

Purpose: ordinary operating preferences plus a collapsed **Advanced / Diagnostics** area that retains the useful controls in the existing test UI.

### Visible reference contents

| Application settings | Controller settings |
|---|---|
| On-screen keyboard policy | Direct command input: ID, value, Set |
| Material clamp rate (%) | Frame-deflection compensation toggle |
| Automatic clamp-to-bend dwell | Ram-to-table opening distance |
| Backgauge retract between bends | Restore factory defaults |
| Backgauge bump-off distance | Cancel and Save |
| Software update; machine/backgauge firmware update | |
| Reset; note that application settings save automatically | |

### Project adaptation

Normal settings: display units; jog/bend speeds; keyboard bindings and visible input-state test; automatic clamp-to-bend option and dwell; X bump-off/retract distances; setup geometry; and saved-data export/import. Proposed clamp percentage behavior must be defined and validated against this machine; a commanded clamp depth cannot confirm actual grip.

Advanced should retain the current backend's real controls:

- Per-motor run current, hold percentage, stationary precharge time.
- Separate vertical/horizontal maximum speed and acceleration; shared start/final speed.
- Maximum steps per command/jog, clearly distinguished from absolute travel limits.
- SGTHRS, DIAG minimum sensing speed, settling time, Stop on DIAG.
- Per-driver selected/seen/configured state, UART address and sample age; SG_RESULT; DIAG level/edge count; raw DRV_STATUS/GSTAT; thermal warnings and faults.
- Apply settings / clear fault, and print applied settings to Serial.
- Connection/ownership status, controller notices, and a readable fault history if implemented.
- Calibration and physical travel bounds as explicit future controller capabilities; settings must not imply enforcement before firmware implements it.

Keep existing validated ranges. Apply motor configuration only while outputs are disabled. Show pending versus confirmed/applied values. Current firmware settings are RAM-only; persistent profiles require an explicit save/reapply design. Selected-driver and pin configuration are currently firmware choices, not live UI toggles. Do not provide an unrestricted raw-command field merely because the reference has one.

## Operating behavior reference from the official manual

The following is a compact manual-derived summary; page inventories above are based on the supplied photographs.

Sequence: optional backgauge positioning → palm-switch approach to clearance → pedal clamping → optional gauge bump-off → pedal bending → automatic retraction. Titan can retract ram and gauge together.

Palm switches require simultaneous operation and release before arming; releasing them pauses approach. Pedal operation uses tap-release-press-and-hold. Releasing during clamp/bend cancels and retracts to clearance. Normally the pedal must release between phases; Auto Clamp-to-Bend allows continued hold through a configurable dwell, including zero dwell.

Modes: Single Bend runs the selection once; Single Bend Repeat repeats it; Program Bend runs from selection to the end; Program Bend Loop repeats the full sequence from the first bend.

Clamp rate defines penetration as a percentage of material thickness. Bump-off withdraws the gauge after clamping; between-bend retract creates removal clearance. Angle correction distinguishes overbent from underbent results.

Source: [Titan operating manual, sections XV and XVIII](https://www.langmuirsystems.com/pages/titan25t-assembly#titan25t-manual-7).

## Intended computer-only control behavior

These are project requirements/proposals, not a claim that an ordinary keyboard supplies safety-rated two-hand protection. Keyboard separation reproduces the interaction; physical machine safeguarding must be independent of browser/key handling.

| Action | Proposed input | Required behavior |
|---|---|---|
| Prepare a program or move | Click Run | Validate setup and show the pending action; no immediate downward movement. |
| Punch down / clamp / bend | Hold Shift + D | Releasing either key stops motion. Require fresh input to resume. |
| Clamp or bend stroke | Hold Shift + ArrowDown | Shift is the requested enable key. Require a fresh action press after preparing each phase. |
| Optional closer Titan pedal emulation | Shift held; tap/release ArrowDown, then press/hold | Explicitly optional; choose and display one gesture consistently. No ambiguous double-tap handling. |
| Retract upward | Hold Shift + ArrowUp, or hold on-screen Retract | Deliberate upward motion within verified bounds. |
| Stop | Escape, Space outside editable fields, persistent Stop button | Cancel sequencing and stop pulses; preserve existing hold behavior where supported. |
| Disable | Explicit Disable Drivers control | Global removal of holding torque; separate from Stop. |

Default Auto Clamp-to-Bend to off. At the completed clamp target, stop and indicate **Clamp position reached**, then request a fresh bend gesture. Do not label this as sensor-confirmed clamping. If automatic transition is later enabled, show its countdown and require the enable gesture throughout.

For the initial stepper implementation, releasing a stroke gesture should **stop and cancel the sequence, retaining hold**, with deliberate retract afterward. This is an intentional difference from Titan's automatic release-triggered retract. Do not launch an additional movement on input loss, fault, or disconnected telemetry. Normal completed-stroke retraction may be a validated sequence step.

Only the focused motion-control context accepts movement keys. Typing into fields, modals, page changes, focus loss, hidden tabs, and disconnects must clear held-key state and cancel pending actions. Ignore operating-system key repeat as a command trigger. No automatic restart after reconnection, fault clearing, or returning focus. Preserve the controller watchdog; a browser key-release handler alone cannot guarantee a stop.

## Existing backend: verified capabilities and gaps

Reviewed `Testing Hardware/09.13_computer_hosted_all_four_motors` source and documentation. Browser → local Node relay → ESP32 over Wi-Fi/WebSocket. ESP32 generates pulses and owns motion/fault checks. UI edits require a browser refresh, not firmware upload.

Available: arm/disable/stop; held jog; finite relative move; per-stage zero; absolute target/return-to-zero; speed ramps; driver telemetry/settings; ownership; heartbeat watchdog. Commands include `jog:v:+`, `step:h:100`, `goto:v:0`, and `zero:h`. Values are commanded full steps; v/h identify vertical/horizontal stages.

Important constraints: (as of Sept 20)

- Only one stage moves at a time; no queued or simultaneous-axis execution. Serialize X and Y operations, including retracts.
- No limit inputs as of Sept 19 but limit switches will be connected soon for physical homing, independent squaring. No encoders though.
- Zeros become invalid on disable/fault/reboot. Setting zero does not establish a physical home.
- All drivers share enable. Disabling or losing the owner connection removes holding torque. Vertical load will never fall on its own, plus it only weighs like 500g.
- The per-command step budget is not an absolute travel envelope; repeated moves can exceed physical travel.
- Electrical notes list horizontal travel as 1200 full steps (48 mm), and vertical travel as 625 full steps. Those are planning data to verify, not currently enforced firmware limits.
- The status protocol reports completion through state/position, not a full job-execution API. A future sequencer must verify an accepted move, expected target, and completion before advancing; an idle snapshot alone is insufficient.

## Scope boundaries for future implementation

The end goal includes all six pages and the complete staged workflow. An initial demo can use calibrated explicit depths, one X reference, a single saved bend, and manual homing with clearly labeled limitations. Unsupported R motion, measured force, automatic angle prediction, and advanced compensation must remain visibly unavailable until actually implemented. The latest user request authorizes checkpoint firmware in a new folder; preserve the old working test and do not silently alter electrical wiring.
