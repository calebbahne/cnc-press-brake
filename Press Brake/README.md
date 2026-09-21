# Press Brake — USB control checkpoint

The hardware UI now uses the ESP32 programming USB cable. The computer still serves the same browser pages at `http://127.0.0.1:8080`; `usb-server.cjs` relays commands, telemetry, and settings through the ESP COM port. The TMC2209 UART on GPIO16/17 is unchanged. Wi-Fi is absent from the active firmware. `server.cjs` remains for the hardware-free simulator and older relay tests.

**Manual commissioning motion:** Manual Mode has one shared speed/distance control and two axis panels. Hold Shift+U/D for the punch and Shift+F/B for the backgauge. It works before home, retains a finite per-command budget, and stops when either key is released. Y is zero at the upper punch reference and negative downward; X is positive toward the die.

**This USB build has not yet been compiled, uploaded, or tested against the machine.** Keep the motor outputs unloaded/disabled for the first link test. The PC relay needs Node dependencies (`npm install` in this folder; the launcher installs them if missing). Close Arduino Serial Monitor before launching the UI because it and the relay use the same COM port. Close the UI window before the next upload.

September 20, 2026. Local six-page control UI, protocol 2 ESP32 firmware, and a hardware-free simulator. The earlier `Testing Hardware` folder is preserved.

**Earlier checkpoint status:** The user compiled and uploaded the first Wi-Fi firmware checkpoint. Controller connection and driver configuration were checked live without commanding motor motion. The new USB firmware has not been compiled or physically tested. No bend has been physically verified.

### Earlier Wi-Fi connection observations

The first hardware upload compiled and the ESP was reachable at `10.0.0.132`. The UI showed **UART/configuration lost** and all four drivers as **UART seen / configuration failed**; this is why Arm was disabled. Reapplying the unchanged settings while outputs were disabled succeeded: all four drivers became READY and Arm became available. No remote arming or motion was performed.

The source retries driver setup up to five times after an initial failure, while outputs remain disabled. It also prints IFCNT and driver register checks at startup, and shows the fault reason in the right panel. If the startup fault appears, open Settings and select **Apply settings / clear fault** while disabled. Only Arm after all installed drivers show READY.

The earlier connection update kept heartbeats going through a briefly delayed status packet and allowed an 8-second heartbeat gap. The USB build retains those deadlines. If a watchdog fault is latched, keep outputs disabled, use **Settings → Apply settings / clear fault** after the USB connection stabilizes, then Arm and establish homes again.

## Open the preview

1. Double-click **Preview UI.cmd**.
2. Open **http://127.0.0.1:8080**. The yellow SIMULATION banner must be visible.
3. Settings → Load simulation example.
4. Punch/Die & Materials → Apply saved depth limit to controller (the example is 10 mm).
5. Arm motors. Manual Mode → Set current spot as home; Back Gauge → Set current spot as home.
6. Run → Prepare single bend. Follow the right-panel gestures through gauge, approach, clamp, bend, retract.

The example depths are arbitrary simulation values, not usable hardware calibration. Simulator libraries and controller profiles use separate browser storage keys from real-machine data. Closing the preview server stops the simulation. You can also run `node server.cjs --simulate 8088` for a separate preview port.

## Upload and connect to hardware

1. In Arduino IDE, open [firmware/press_brake/press_brake.ino](firmware/press_brake/press_brake.ino). Use DOIT ESP32 DEVKIT V1 and TMCStepper. The active sketch does not need the WebSockets library, Wi-Fi headers, or `wifi_secrets.h`.
2. Confirm all four driver-present flags match physically installed drivers. Y is negative down and X is positive toward the die. Verify with a small unloaded distance jog before homing. If a pair moves in the wrong direction, correct the corresponding firmware inversion configuration before proceeding.
3. Compile/upload in Arduino IDE. If inspecting boot diagnostics in Serial Monitor, use **115200 baud**, then close Serial Monitor. No Arduino toolchain was available for this checkpoint.
4. Find the ESP COM port in Windows Device Manager. Close the preview server if it uses port 8080. Double-click **Start UI.cmd**, enter that COM port (for example `COM5`), and open **http://127.0.0.1:8080**. Keep the launcher window open. It prints ESP diagnostics and retries a temporarily unavailable port.
5. The UI refuses older firmware: it requires protocol 3 and reports the steps/mm derived from the applied microstep setting. No SIMULATION banner should appear when connected to the ESP.

## First real session: manual home, no DIAG wires

1. Leave **Use installed mechanical limit switches** and **Stop on DIAG** unchecked. UART protection still works without DIAG wires; DIAG interrupts are detached while disabled.
2. Enter punch/die part numbers, dimensions and usable lengths. Default material is **0.2 mm aluminum flashing**. Save the setup.
3. Measure a conservative deepest permitted punch position from your intended repeatable physical home. Enter that as **Deepest permitted Y**, above the point where tooling could collide. This is manually measured; part dimensions do not automatically derive a collision envelope.
4. While disabled, apply the saved depth limit to the controller before running bend programs. Review machine travel and speeds under Settings. The initial 0 mm tool cap blocks programmed downward travel until you set a limit; it does not block Manual Mode motion.
5. Arm. Before home, Manual Mode and Back Gauge permit held setup increments up to **1 mm per command** and held keyboard jog. Start with **0.1 mm** increments to check direction. These unhomed commissioning moves cannot enforce an unknown absolute endpoint.
6. Position Y at your repeatable retracted reference and X at your repeatable retracted gauge reference. Stop and use **Set current spot as home** on each page. Position is counted from these references; no encoders are planned.
7. In Manual Mode, approach a scrap coupon in small increments. Record the clearance position, initial contact/clamp position, and a tested bend depth. A reported clamp position does not prove grip or force. Measure the resulting angle and adjust the depth using test pieces.
8. Create Bend: enter width, desired included angle, taught approach/clamp/final/retract positions, optional X position, and calibration notes. Check the calibration confirmation and Save. Use the same tooling, material and home reference used for calibration.
9. Run → Prepare single bend. It validates the setup, travel caps, command distances, and home validity before enabling the gesture for the first phase.

Changing width or desired angle does **not** calculate a new depth. Calibrate and save separate recipes as needed. A future model can build on these measurements. No force/tonnage estimate is presented as measured load.

## Controls

| Action | Input / behavior |
|---|---|
| Prepare | Click Prepare Single Bend. No movement starts yet. |
| Punch up / retract | Hold **Shift + U**. |
| Punch down / clamp / bend | Hold **Shift + D**. |
| Backgauge toward die | Hold **Shift + F**. |
| Backgauge away from die | Hold **Shift + B**. |
| Manual Mode | Choose continuous speed or distance per hold, choose mm or steps, then hold the matching Shift shortcut. Release either key to stop. |
| Phase change | Release all motion keys; hold a fresh gesture for the next phase. |
| Release during motion | Stop and cancel the prepared sequence. Re-prepare to continue. |
| Stop | Escape anywhere, Space outside editable fields, or persistent Stop button. Retains hold current. |
| Disable | Removes driver enable and clears both home references. |
| Lost focus / hidden tab | Stops, requests disable, clears key states. No automatic resume. |

The controller requires a continuing **motion hold lease (350 ms)** in addition to its **connection heartbeat (8 seconds in this update)**. The browser sends the heartbeat while armed even when a status packet is briefly delayed; an active move still stops after 1.5 seconds without telemetry, or within 350 ms without the separate motion hold. Held jogs use a per-command move budget, but no assumed home or software travel endpoint. A browser freeze cannot keep a move running indefinitely merely because a connection exists. Watchdog/fault disable clears homes. An ordinary keyboard remains an operator control, not a safety-rated two-hand switch or hardware emergency stop.

Automatic clamp-to-bend, pedal double-tap, unattended repeats, multi-bend sequencing and automatic retract after a released gesture are intentionally not enabled in this checkpoint. Every stroke phase requires a deliberate hold; both stage pairs move sequentially.

## Adding limit switches later

- Wire NC switches per [electrical quick reference](../electrical-quick-reference.md): Y1 GPIO34, Y2 GPIO35, X GPIO36, external pull-ups. LOW is closed; HIGH is triggered or an open wire. The inputs lack internal pull-ups.
- Disable outputs, check the wiring, enable switch homing in Settings, and Apply. Check all three displayed inputs change independently before commanding home.
- Enable Y and X switch homing independently in Settings. With only the two punch switches installed, enable **Y switch homing** and leave **X switch homing** off. Prepare Y home and hold **Shift + U**; X remains available for manual home.
- Y seeks each switch independently, stopping that side's steps while the other approaches. X uses its shared STEP and one home switch; independent X squaring is unavailable.
- Homing has a speed, total seek budget, 120-second ceiling, maximum Y squaring correction (default 1 mm), and backoff (default 1 mm). Excess correction, an unstable/open input, failure to reach a switch, or failure to release after backoff faults rather than declaring home.
- Home zero is the switch trip location. On successful Y backoff, the displayed position is negative because the punch moved downward from the upper zero. During initial seek, unhomed coordinate readouts are not independent measurements of the two ram ends.
- These are home switches, not switches at both travel ends. Motion toward a triggered home input is stopped when switch homing is enabled. Manual commissioning motion otherwise has no software endpoint; programmed bends retain their configured bounds.
- Until wiring and direction are physically verified, keep switch homing off. The implementation needs hardware commissioning; simulation does not validate switch polarity, debounce, squaring or mechanics.

## Calibration and limits

- Microstepping is selectable in Settings from 1× through 256×. Steps/mm is `25 × microsteps`; the default 8× setting is 200 steps/mm or 0.005 mm commanded resolution. Applying a change reconfigures all selected TMC2209 drivers and clears homes. Pulse based speed, acceleration, travel and backoff values may need adjustment.
- Driver checks compare the configured microstep register value and interpolation state, and the StallGuard speed threshold scales with the selected microstepping.
- Documented physical travel gives upper bounds of Y **25 mm / 5000 pulses** and X **48 mm / 9600 pulses**. Reduce these to the actual usable machine travel from the chosen home; firmware rejects settings above these maxima.
- The installed tool maximum restricts programmed bends. Manual commissioning motion ignores that assumed envelope, including motion to a negative position relative to declared home. Use small increments until the actual travel and clearances are measured.
- All four drivers share EN. Y normally steps both selected vertical drivers; independent Y stepping is used only during switch homing. X always shares pulses.
- Use **the same physical manual home each session**. Re-zeroing at a different height moves every saved tooling depth and calibration relative to the machine. Counts cannot detect slip or missed steps.

## USB link

The browser talks only to the local Node server. The server opens the ESP COM port at 115200 baud and relays browser commands as framed serial lines. Status and command results return on the same cable. Driver diagnostics appear in the Start UI window. Status is sent every 300 ms. The 8-second armed connection watchdog and independent 350 ms motion-hold lease remain in the firmware. A cable unplug, PC exit, or browser loss must stop motion; after a fault, apply settings while disabled, then Arm and establish homes again. The ESP can remain powered from the machine supply after USB is unplugged, so the firmware deadlines still govern it.

## Files and future hosting

- `ui/index.html`, `style.css`, `app.js`: local presentation and operator interaction.
- `ui/model.js`: recipe validation and command-completion checks, shared with tests.
- `usb-server.cjs`: local static host and USB serial relay. It never generates heartbeats or queues motion.
- `server.cjs`: network relay retained for the simulator preview.
- `firmware/press_brake`: controller motion, homing, driver protection, bounds and USB protocol. The old Wi-Fi files are inactive.
- `simulator.cjs`: protocol simulator, reachable only through the local preview. It cannot send hardware commands.

The local host binds only to 127.0.0.1. No public remote control or sign-in has been added.

## Verification and next checkpoint

Run from this folder:

```powershell
node --test usb-server.test.cjs server.test.cjs model.test.cjs simulator.test.cjs controls.test.cjs
```

38 tests cover recipe validation, programmed bounds, manual commissioning motion, acknowledgement/completion, keyboard holds/release/focus, simulated sequential moves, hold expiry, USB relay and disconnect, network preview relay, and isolation of preview storage. The simulator models protocol behavior; it does not execute or certify the ESP firmware.

Next checkpoint requires Arduino IDE upload and unloaded physical checks: USB connection and reconnect, settings apply, correct directions and 1 mm travel calibration, manual homes, tool/machine bound rejection, release/Stop/focus-loss behavior, then a scrap-material bend. Do not treat this checkpoint as a validated automatic bend-angle controller.
