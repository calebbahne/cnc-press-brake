# Press Brake — Checkpoint 01

September 20, 2026. Local six-page control UI, protocol 2 ESP32 firmware, and a hardware-free simulator. The earlier `Testing Hardware` folder is preserved.

**Status:** UI, planning, relay, keyboard handlers, and simulated protocol tested locally. Firmware has been reviewed but **not compiled, uploaded, or physically tested**. Arduino compilation was intentionally not attempted at Caleb's request. This checkpoint is ready for that hardware validation, not a claim that a bend has already been made.

## Open the preview

1. Double-click **Preview UI.cmd**.
2. Open **http://127.0.0.1:8080**. The yellow SIMULATION banner must be visible.
3. Settings → Load simulation example.
4. Punch/Die & Materials → Apply saved depth limit to controller (the example is 10 mm).
5. Arm motors. Manual Mode → Set current spot as home; Back Gauge → Set current spot as home.
6. Run → Prepare single bend. Follow the right-panel gestures through gauge, approach, clamp, bend, retract.

The example depths are arbitrary simulation values, not usable hardware calibration. Simulator libraries and controller profiles use separate browser storage keys from real-machine data. Closing the preview server stops the simulation. You can also run `node server.cjs --simulate 8088` for a separate preview port.

## Upload and connect to hardware

1. In Arduino IDE, open [firmware/press_brake/press_brake.ino](firmware/press_brake/press_brake.ino). The folder and sketch names match. Use the same ESP32 board and libraries as the existing test: DOIT ESP32 DEVKIT V1, TMCStepper, and Markus Sattler's WebSockets. Preferences, WiFi, WebServer and esp_timer come with ESP32 support.
2. Keep `network_setup.h` and the local `wifi_secrets.h` beside the sketch. Your two network preferences are already in the local secrets file, which is excluded from Git. An example secrets header is supplied for another checkout. Do not upload credentials to public hosting.
3. Confirm all four driver-present flags match physically installed drivers. Existing inversion flags were retained; positive logical motion is labeled Y down and X toward the die. Verify with a small unloaded jog before homing. If a pair moves in the wrong direction, correct the corresponding firmware inversion configuration before proceeding.
4. Compile/upload in Arduino IDE. Open Serial Monitor at 115200 for configuration status and the ESP IP address. Report compiler errors if any; no Arduino toolchain has been run on this checkpoint.
5. Close the preview server if it uses port 8080. Double-click **Start UI.cmd**, enter the ESP IP (or try `cnc-press-brake.local`), and open **http://127.0.0.1:8080**.
6. The UI refuses older firmware: it requires protocol 2 and 200 pulses/mm. No SIMULATION banner should appear when connected to the ESP.

## First real session: manual home, no DIAG wires

1. Leave **Use installed mechanical limit switches** and **Stop on DIAG** unchecked. UART protection still works without DIAG wires; DIAG interrupts are detached while disabled.
2. Enter punch/die part numbers, dimensions and usable lengths. Default material is **0.2 mm aluminum flashing**. Save the setup.
3. Measure a conservative deepest permitted punch position from your intended repeatable physical home. Enter that as **Deepest permitted Y**, above the point where tooling could collide. This is manually measured; part dimensions do not automatically derive a collision envelope.
4. While disabled, apply the saved depth limit to the controller. Review machine travel and speeds under Settings. The initial 0 mm tool cap intentionally blocks normal downward travel after home until you set a limit.
5. Arm. Before home, Manual Mode and Back Gauge permit held setup increments up to **1 mm per command**; start with **0.1 mm** to check direction. These unhomed commissioning moves cannot enforce an unknown absolute endpoint.
6. Position Y at your repeatable retracted reference and X at your repeatable retracted gauge reference. Stop and use **Set current spot as home** on each page. Position is counted from these references; no encoders are planned.
7. In Manual Mode, approach a scrap coupon in small increments. Record the clearance position, initial contact/clamp position, and a tested bend depth. A reported clamp position does not prove grip or force. Measure the resulting angle and adjust the depth using test pieces.
8. Create Bend: enter width, desired included angle, taught approach/clamp/final/retract positions, optional X position, and calibration notes. Check the calibration confirmation and Save. Use the same tooling, material and home reference used for calibration.
9. Run → Prepare single bend. It validates the setup, travel caps, command distances, and home validity before enabling the gesture for the first phase.

Changing width or desired angle does **not** calculate a new depth. Calibrate and save separate recipes as needed. A future model can build on these measurements. No force/tonnage estimate is presented as measured load.

## Controls

| Action | Input / behavior |
|---|---|
| Prepare | Click a move or Prepare Single Bend. No movement starts yet. |
| Gauge positioning / punch approach | Hold **A + L**. |
| Clamp / bend | Hold **Shift + Down arrow**. |
| Retract | Hold **Shift + Up arrow**. |
| Manual increment / absolute move | Prepare, then hold the indicated gesture. The on-screen Hold button supports upward Y and X moves; downward Y requires A + L. |
| Phase change | Release all motion keys; hold a fresh gesture for the next phase. |
| Release during motion | Stop and cancel the prepared sequence. Re-prepare to continue. |
| Stop | Escape anywhere, Space outside editable fields, or persistent Stop button. Retains hold current. |
| Disable | Removes driver enable and clears both home references. |
| Lost focus / hidden tab | Stops, requests disable, clears key states. No automatic resume. |

The controller requires a continuing **motion hold lease (350 ms)** in addition to its **connection heartbeat (2 seconds, inherited from the current test sketch)**. A browser freeze cannot keep a finite move running indefinitely merely because a connection exists. Watchdog/fault disable clears homes. An ordinary keyboard remains an operator control, not a safety-rated two-hand switch or hardware emergency stop.

Automatic clamp-to-bend, pedal double-tap, unattended repeats, multi-bend sequencing and automatic retract after a released gesture are intentionally not enabled in this checkpoint. Every stroke phase requires a deliberate hold; both stage pairs move sequentially.

## Adding limit switches later

- Wire NC switches per [electrical quick reference](../electrical-quick-reference.md): Y1 GPIO34, Y2 GPIO35, X GPIO36, external pull-ups. LOW is closed; HIGH is triggered or an open wire. The inputs lack internal pull-ups.
- Disable outputs, check the wiring, enable switch homing in Settings, and Apply. Check all three displayed inputs change independently before commanding home.
- **Prepare switch home** on each axis, then hold A + L. Both home directions are logical negative: Y up, X away from the die. Both vertical drivers must be installed. A switch already high blocks starting home; check wiring and, when appropriate, jog away first.
- Y seeks each switch independently, stopping that side's steps while the other approaches. X uses its shared STEP and one home switch; independent X squaring is unavailable.
- Homing has a speed, total seek budget, 120-second ceiling, maximum Y squaring correction (default 1 mm), and backoff (default 1 mm). Excess correction, an unstable/open input, failure to reach a switch, or failure to release after backoff faults rather than declaring home.
- Home zero is the switch trip location. On successful backoff, the displayed position is the positive backoff distance, not zero. During initial seek, unhomed coordinate readouts are not independent measurements of the two ram ends.
- These are home switches, not switches at both travel ends. Normal travel toward a triggered home input is stopped; software bounds govern the other end after home.
- Until wiring and direction are physically verified, keep switch homing off. The implementation needs hardware commissioning; simulation does not validate switch polarity, debounce, squaring or mechanics.

## Calibration and limits

- Current source uses **8× microsteps** with interpolation: 200 motor full steps/revolution × 8 / 8 mm lead = **200 STEP pulses/mm**, or 0.005 mm commanded resolution. This is not measured accuracy.
- The original copied source set 8× microsteps but checked the full-step register setting. The new source checks the 8× register value and interpolation state consistently, and scales the StallGuard speed threshold accordingly.
- Documented physical travel gives upper bounds of Y **25 mm / 5000 pulses** and X **48 mm / 9600 pulses**. Reduce these to the actual usable machine travel from the chosen home; firmware rejects settings above these maxima.
- The installed tool maximum further restricts Y. Firmware enforces it for homed jog/increment/absolute motion, not just the browser's Run page. Unhomed setup increments necessarily lack an absolute envelope; use them only to establish the real reference.
- All four drivers share EN. Y normally steps both selected vertical drivers; independent Y stepping is used only during switch homing. X always shares pulses.
- Use **the same physical manual home each session**. Re-zeroing at a different height moves every saved tooling depth and calibration relative to the machine. Counts cannot detect slip or missed steps.

## Wi-Fi fallback

Boot tries the existing preferred network, then **iPhone (8)**, then a custom saved network, allowing 15 seconds per candidate. The iPhone password supplied in chat is installed in the ignored local secrets header.

If none connect, the ESP creates **PressBrake-Setup**. Its local setup password is in `wifi_secrets.h` (`SETUP_AP_PASSWORD`). Connect your computer/phone to it and browse **http://192.168.4.1**. Enter the new SSID/password, save, and the ESP restarts. The custom network persists in ESP NVS as the third preference. Motion is disabled in setup mode. This is an explicit setup page, not an automatically opening captive portal.

Rejoin the selected normal network afterward and launch the PC host with the ESP's new address from Serial. If all candidates fail, the setup network returns. The network connection state machine does not block the motion-service loop with connection-wait delays.

Implementation references: [Espressif Wi-Fi API](https://docs.espressif.com/projects/arduino-esp32/en/latest/api/wifi.html) for station/AP operation and [Preferences API](https://docs.espressif.com/projects/arduino-esp32/en/latest/api/preferences.html) for saved credentials.

## Files and future hosting

- `ui/index.html`, `style.css`, `app.js`: local presentation and operator interaction.
- `ui/model.js`: recipe validation and command-completion checks, shared with tests.
- `server.cjs`: local static host and transparent WebSocket/HTTP relay. It never generates heartbeats or queues motion.
- `firmware/press_brake`: controller motion, homing, driver protection, bounds and network provisioning.
- `simulator.cjs`: protocol simulator, reachable only through the local preview. It cannot send hardware commands.

The local host binds only to 127.0.0.1. The ESP API remains intended for a trusted local network; no public remote control or sign-in has been added. A future Firebase-hosted UI needs a separate authenticated, encrypted connection path to a local gateway or outbound controller connection, plus authorization and local control ownership. A cloud sign-in page alone would not secure the existing plain local WebSocket API. Preserve controller-side deadlines and limits in any later network architecture.

## Verification and next checkpoint

Run from this folder:

```powershell
node --test server.test.cjs model.test.cjs simulator.test.cjs controls.test.cjs
```

30 tests cover recipe validation, bounds, acknowledgement/completion, keyboard holds/release/focus, simulated sequential moves, hold expiry, relay/origin handling and isolation of preview storage. The simulator models protocol behavior; it does not execute or certify the ESP firmware. Browser inspection verified page rendering, settings apply, and the saved-demo display.

Next checkpoint requires Arduino IDE upload and unloaded physical checks: correct directions and 1 mm travel calibration, manual homes, tool/machine bound rejection, release/Stop/focus-loss behavior, then a scrap-material bend. Switch homing and Wi-Fi provisioning need separate hardware checks when those facilities are available. Do not treat this checkpoint as a validated automatic bend-angle controller.
