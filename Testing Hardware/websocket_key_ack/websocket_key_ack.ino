/*
  websocket_key_ack.ino

  Baseline ESP32 WebSocket/keyboard test.

  1. Set WIFI_SSID and WIFI_PASSWORD below, upload to the ESP32, then open the
     Serial Monitor at 115200 baud.
  2. Browse to http://cnc-press-brake.local (or use the printed IP as a fallback).
  3. Click the page once, then hold Shift with U or D. The page sends key-down,
     periodic hold heartbeats, and key-up messages over a WebSocket. The ESP
     replies with an acknowledgement displayed in the page and Serial Monitor.

  Required Arduino libraries: WiFi (included with ESP32 boards), WebServer,
  and WebSockets by Markus Sattler ("WebSockets" in Library Manager).

  This sketch does NOT enable drivers or move any motor.
*/

#include <WiFi.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <ESPmDNS.h>

constexpr char MDNS_HOSTNAME[] = "cnc-press-brake"; // http://cnc-press-brake.local

const char *WIFI_SSID = "Rhymes with Donna";
const char *WIFI_PASSWORD = "!Bbrosgaming2020";

WebServer httpServer(80);
WebSocketsServer webSocket(81);

const char CONTROL_PAGE[] PROGMEM = R"HTML(
<!doctype html><html><head><meta name="viewport" content="width=device-width,initial-scale=1">
<style>body{font:18px sans-serif;max-width:42rem;margin:2rem auto;padding:0 1rem}kbd{border:1px solid #777;padding:.2rem .5rem;border-radius:.25rem}#state{padding:1rem;background:#eee}</style>
</head><body><h1>ESP32 keyboard acknowledgement test</h1>
  <p>Click this page, then hold <kbd>Shift</kbd> with <kbd>U</kbd> or <kbd>D</kbd>. Release either key to stop its heartbeat.</p>
<p id="state">Connecting…</p><script>
let ws, held = '', shiftHeld = false;
const state = document.getElementById('state');
function send(message) { if (ws && ws.readyState === WebSocket.OPEN) ws.send(message); }
function connect() {
  ws = new WebSocket(`ws://${location.hostname}:81/`);
  ws.onopen = () => state.textContent = 'Connected — press U or D.';
  ws.onmessage = e => state.textContent = e.data;
  ws.onclose = () => { state.textContent = 'Disconnected; retrying…'; setTimeout(connect, 1000); };
}
addEventListener('keydown', e => {
  const key = e.key.toLowerCase();
  if (key === 'shift' && !shiftHeld) { shiftHeld = true; send('enable:shift:down'); e.preventDefault(); return; }
  if ((key === 'u' || key === 'd') && held !== key) { held = key; send('key:' + key + ':down'); e.preventDefault(); }
});
addEventListener('keyup', e => {
  const key = e.key.toLowerCase();
  if (key === 'shift' && shiftHeld) { shiftHeld = false; send('enable:shift:up'); e.preventDefault(); return; }
  if (key === held) { send('key:' + key + ':up'); held = ''; e.preventDefault(); }
});
addEventListener('blur', () => { if (held) send('key:' + held + ':up'); if (shiftHeld) send('enable:shift:up'); held = ''; shiftHeld = false; });
setInterval(() => { if (held && shiftHeld) send('key:' + held + ':hold'); }, 100);
connect();
</script></body></html>
)HTML";

void sendAck(uint8_t client, const String &message) {
  String ack = "ESP32 received: " + message;
  Serial.println(ack);
  webSocket.sendTXT(client, ack);
}

void onWebSocketEvent(uint8_t client, WStype_t type, uint8_t *payload, size_t length) {
  if (type == WStype_CONNECTED) {
    sendAck(client, "WebSocket connected");
  } else if (type == WStype_TEXT) {
    String message;
    message.reserve(length);
    for (size_t i = 0; i < length; ++i) message += (char)payload[i];
    sendAck(client, message);
  }
}

void setup() {
  Serial.begin(115200);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting to Wi-Fi");
  while (WiFi.status() != WL_CONNECTED) { delay(250); Serial.print('.'); }
  if (MDNS.begin(MDNS_HOSTNAME)) {
    MDNS.addService("http", "tcp", 80);
    Serial.printf("\nOpen http://%s.local in a browser\n", MDNS_HOSTNAME);
  } else {
    Serial.printf("\nmDNS failed; open http://%s in a browser\n", WiFi.localIP().toString().c_str());
  }

  httpServer.on("/", []() { httpServer.send_P(200, "text/html", CONTROL_PAGE); });
  httpServer.begin();
  webSocket.begin();
  webSocket.onEvent(onWebSocketEvent);
}

void loop() {
  httpServer.handleClient();
  webSocket.loop();
}
