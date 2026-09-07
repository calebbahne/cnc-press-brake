/*
  websocket_key_ack.ino

  Baseline ESP32 WebSocket/keyboard test.

  1. Set WIFI_SSID and WIFI_PASSWORD below, upload to the ESP32, then open the
     Serial Monitor at 115200 baud.
  2. Browse to the IP address printed by the ESP32.
  3. Click the page once, then hold U or D.  The page sends key-down, periodic
     hold heartbeats, and key-up messages over a WebSocket.  The ESP replies
     with an acknowledgement displayed in the page and Serial Monitor.

  Required Arduino libraries: WiFi (included with ESP32 boards), WebServer,
  and WebSockets by Markus Sattler ("WebSockets" in Library Manager).

  This sketch does NOT enable drivers or move any motor.
*/

#include <WiFi.h>
#include <WebServer.h>
#include <WebSocketsServer.h>

const char *WIFI_SSID = "REPLACE_WITH_WIFI_NAME";
const char *WIFI_PASSWORD = "REPLACE_WITH_WIFI_PASSWORD";

WebServer httpServer(80);
WebSocketsServer webSocket(81);

const char CONTROL_PAGE[] PROGMEM = R"HTML(
<!doctype html><html><head><meta name="viewport" content="width=device-width,initial-scale=1">
<style>body{font:18px sans-serif;max-width:42rem;margin:2rem auto;padding:0 1rem}kbd{border:1px solid #777;padding:.2rem .5rem;border-radius:.25rem}#state{padding:1rem;background:#eee}</style>
</head><body><h1>ESP32 keyboard acknowledgement test</h1>
<p>Click this page, then hold <kbd>U</kbd> or <kbd>D</kbd>. Release the key to stop its heartbeat.</p>
<p id="state">Connecting…</p><script>
let ws, held = '';
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
  if ((key === 'u' || key === 'd') && held !== key) { held = key; send('key:' + key + ':down'); e.preventDefault(); }
});
addEventListener('keyup', e => {
  const key = e.key.toLowerCase();
  if (key === held) { send('key:' + key + ':up'); held = ''; e.preventDefault(); }
});
addEventListener('blur', () => { if (held) send('key:' + held + ':up'); held = ''; });
setInterval(() => { if (held) send('key:' + held + ':hold'); }, 100);
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
  Serial.printf("\nOpen http://%s in a browser\n", WiFi.localIP().toString().c_str());

  httpServer.on("/", []() { httpServer.send_P(200, "text/html", CONTROL_PAGE); });
  httpServer.begin();
  webSocket.begin();
  webSocket.onEvent(onWebSocketEvent);
}

void loop() {
  httpServer.handleClient();
  webSocket.loop();
}
