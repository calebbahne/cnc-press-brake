# Computer-hosted four-motor test

The PC serves the UI at **http://127.0.0.1:8080**. The browser sends commands
through the Node.js server to the ESP over the existing Wi-Fi network.
The ESP still generates every pulse, ramps motion, tracks commanded positions,
checks drivers, enforces command limits, and handles watchdog/fault stops.
UI changes now require a browser refresh, not a firmware upload.

## Start

1. Open `09.13_computer_hosted_all_four_motors.ino` in Arduino IDE and upload
   to your ESP32. Board: DOIT ESP32 DEVKIT V1. Libraries: TMCStepper and
   WebSockets (Markus Sattler). Existing Wi-Fi configuration is retained.
   Current selection is **vertical 1/2 disabled; horizontal 1/2 enabled**,
   matching the source sketch. Adjust the four CONNECTED switches to match
   physically installed drivers before uploading.
2. Read the ESP IP in Serial Monitor at 115200 baud.
3. Double-click **Start UI.cmd**. Enter that IP, or press Enter to try
   `cnc-press-brake.local`. Node.js must be installed (it is present on this PC).
4. Open **http://127.0.0.1:8080** in your browser. Keep the server window running.
   Check driver status, Apply / clear fault if needed, then Arm explicitly.

From a terminal in this folder: `node server.cjs 192.168.1.123` (substitute
the ESP IP). An optional final argument selects another local port, e.g.
`node server.cjs 192.168.1.123 8081`. Stop with Ctrl+C. The host listens only on
127.0.0.1; no incoming firewall rule is needed. Use the exact 127.0.0.1 URL.

## Files and communication

- `ui/index.html`: editable PC-side page, styling, and browser logic.
- `server.cjs`: dependency-free static server and HTTP/WebSocket relay.
- The `.ino`: ESP motion firmware, with the embedded page removed.
- `HARDWARE.md`: inherited wiring, controls, settings, and hardware limitations.
  Its original startup/validation paragraphs describe the source test;
  use this README for this version's startup and validation.

The relay forwards settings to ESP port 80 and one WebSocket per browser to
ESP port 81. It does not generate heartbeats, queue commands, or reconnect
motion automatically. The browser retries its connection; re-arm and re-zero
are required after a lost owner connection. The ESP still owns the 750 ms
watchdog. Closing/hiding the owner browser requests disable; losing the
server/network also causes the ESP to stop and disable. Disabling removes
holding torque, so support vertical loads. This remains a bench test with no
limits, automatic homing, or measured position feedback.

If disconnected: verify the ESP has firmware running, both devices are on
the same reachable network, and restart the PC server with the numeric IP if
mDNS resolution fails. `/health` checks only the PC server, not the ESP.
Existing direct ESP APIs remain unauthenticated; use the trusted local network.

## Validation

Run `node --test server.test.cjs` for local simulated-ESP relay checks.
These cover page serving, settings and error forwarding, origin rejection,
oversized requests, offline ESP behavior, WebSocket byte forwarding, and
disconnect propagation. Firmware compilation, upload, and physical motion
verification are separate bench steps; they have not been performed here.
