# ESP32 Wi-Fi-only diagnostic

This sketch tests **Rhymes with Donna only** without the press-brake firmware, TMC UART traffic, WebSockets, or motor commands. GPIO27 (shared active-low driver enable) is set HIGH; no STEP pulses are generated. The supplied Wi-Fi credentials are embedded in this local sketch; do not publish it. The password is not printed to Serial.

1. Open `09.20_wifi_connection_diagnostic.ino` in Arduino IDE and upload it to the same ESP32. Open Serial Monitor at **115200 baud**. No typing is required.
2. Read `TARGET FOUND` or `TARGET NOT FOUND`. The classic ESP32 needs a compatible **2.4 GHz** access point. If the target is not found, check that the router is on, broadcasting the exact SSID, and close enough.
3. The sketch automatically joins that one network. It prints association and IP events, then either `CONNECTED` or `JOIN FAILED` with a disconnect reason code. It reports status every five seconds so you can see later drops. Reset the ESP to repeat the test.
4. If connected, open the exact `http://<IP>/` printed in Serial Monitor on the **computer**. If the ESP connects but the page does not load, check that the computer is on the same reachable network; a hotspot/router may isolate clients. If the page loads, basic ESP Wi-Fi and PC-to-ESP HTTP are working, and the remaining fault is in the full firmware, WebSocket relay, driver polling, or electrical load.
5. Copy the `TARGET`, `EVENT`, and `JOIN FAILED` or `CONNECTED` lines, and whether the HTTP page loaded. Do not include the password. Re-upload the press-brake sketch afterward; this diagnostic does not run the brake.

The four `UART PASS` lines from the isolated test demonstrate that its UART wiring and addresses answered under that test. They do not establish that the ESP joined Wi-Fi, or that the full controller configured and polled the drivers successfully. The present press-brake log shows both Wi-Fi preferences timing out and setup AP mode starting, which explains missing UI telemetry on that boot. A stale UI can also result from the computer relay pointing at an old ESP IP. After any successful connection, use the **IP printed by the current boot** in the PC launcher.

### September 20 observed result

The preferred network appeared on channel 6 with RSSI **-73 dBm**, but the ESP never received an IP. Disconnect reasons were first **15** (WPA four-way handshake timeout) and **202** (authentication failure), then **2** (authentication phase expired). Reason 2 means the station did not receive an authentication response in time or the access point sent that reason; it is not proof of a wrong password. The next discriminating test is to run the same sketch and credentials with the ESP close to the access point and compare scan RSSI and reason codes. If it still fails with a strong signal, inspect access point security and device restrictions; then test a separate nearby 2.4 GHz WPA2 network to distinguish the ESP radio from that router.
