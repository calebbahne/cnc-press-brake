/*
  vertical_stepper_websocket_test.ino

  PURPOSE: Manual vertical-stepper test only. It controls the shared vertical
  STEP/DIR signals (GPIO 25/26) and active-low global enable (GPIO 27). This
  means BOTH vertical motors receive the same motion command.

  BEFORE POWERING MOTORS:
  - Set WIFI_SSID, WIFI_PASSWORD, and confirm VERTICAL_DIR_UP_LEVEL. Start at
    a low MAX_SPEED_STEPS_PER_SECOND and verify which way "up" actually moves.
  - This is a bring-up test with no limit switches, DIAG, or TMC UART connected.
    Keep clear of the machine, be ready to remove motor power, and do not use it
    on a machine that can contact tooling or reach a mechanical hard stop.
  - Upload, open Serial Monitor (115200), then browse to the printed IP address.
    Click the page to give it keyboard focus.

  CONTROLS:
    Hold Shift + U = up. Release either key = stop.
    Hold Shift + D = down. Release either key = stop.
    M = enter a new maximum speed in the browser prompt (steps/second).
    H = open the on-screen help.

  A 250 ms WebSocket dead-man timeout disables the drivers if communication or
  the browser stops sending heartbeats. Required libraries: AccelStepper and
  WebSockets by Markus Sattler; WiFi/WebServer come with ESP32 boards.
*/

#include <WiFi.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <AccelStepper.h>

const char *WIFI_SSID = "REPLACE_WITH_WIFI_NAME";
const char *WIFI_PASSWORD = "REPLACE_WITH_WIFI_PASSWORD";

constexpr uint8_t VERTICAL_STEP_PIN = 25;
constexpr uint8_t VERTICAL_DIR_PIN = 26;
constexpr uint8_t DRIVER_ENABLE_PIN = 27;  // TMC2209 EN is active-low.
constexpr bool VERTICAL_DIR_UP_LEVEL = HIGH; // Change to LOW if U moves down.
constexpr float ACCELERATION_STEPS_PER_SECOND2 = 800.0f;
constexpr float MIN_SPEED_STEPS_PER_SECOND = 10.0f;
constexpr float MAX_ALLOWED_SPEED_STEPS_PER_SECOND = 5000.0f;
constexpr unsigned long DEADMAN_TIMEOUT_MS = 250;
constexpr long CONTINUOUS_TARGET_STEPS = 100000000L;

WebServer httpServer(80);
WebSocketsServer webSocket(81);
AccelStepper verticalStepper(AccelStepper::DRIVER, VERTICAL_STEP_PIN, VERTICAL_DIR_PIN);

float maxSpeed = 400.0f;
int8_t commandedDirection = 0; // +1 up, -1 down, 0 stopped
bool shiftEnableHeld = false;
unsigned long lastHoldMs = 0;

const char CONTROL_PAGE[] PROGMEM = R"HTML(
<!doctype html><html><head><meta name="viewport" content="width=device-width,initial-scale=1">
<style>body{font:18px sans-serif;max-width:46rem;margin:2rem auto;padding:0 1rem}kbd{border:1px solid #777;padding:.2rem .5rem;border-radius:.25rem}#state{padding:1rem;background:#eee;white-space:pre-wrap}</style>
</head><body><h1>Vertical stage stepper test</h1>
<p>Click here first. Hold <kbd>Shift</kbd> + <kbd>U</kbd> for up, or <kbd>Shift</kbd> + <kbd>D</kbd> for down. Releasing either key stops. <kbd>M</kbd> changes maximum speed; <kbd>H</kbd> shows help.</p>
<p id="state">Connecting…</p><script>
let ws, direction = '', shiftHeld = false; const state = document.getElementById('state');
function send(m) { if (ws && ws.readyState === WebSocket.OPEN) ws.send(m); }
function stop() { send('motion:stop'); }
function updateMotion() { if (shiftHeld && direction) send('motion:'+direction); else stop(); }
function connect() { ws = new WebSocket(`ws://${location.hostname}:81/`); ws.onopen=()=>state.textContent='Connected — hold Shift plus U or D to enable motion.'; ws.onmessage=e=>state.textContent=e.data; ws.onclose=()=>{state.textContent='Disconnected — drivers should be disabled. Retrying…';setTimeout(connect,1000)}; }
addEventListener('keydown', e => { const k=e.key.toLowerCase(); if(k==='shift'&&!shiftHeld) { shiftHeld=true; send('enable:shift:down'); updateMotion(); e.preventDefault(); } else if((k==='u'||k==='d')&&direction!==(k==='u'?'up':'down')) { direction=k==='u'?'up':'down'; updateMotion(); e.preventDefault(); } else if(k==='m'&&!e.repeat) { const v=prompt('Maximum speed (steps/second):'); if(v!==null) send('speed:'+v); e.preventDefault(); } else if(k==='h'&&!e.repeat) { alert('Hold Shift + U: move up\nHold Shift + D: move down\nRelease either key: stop\nM: change maximum speed\nH: this help'); e.preventDefault(); }});
addEventListener('keyup', e => { const k=e.key.toLowerCase(); if(k==='shift') { shiftHeld=false; send('enable:shift:up'); stop(); e.preventDefault(); } else if((k==='u'&&direction==='up')||(k==='d'&&direction==='down')) { direction=''; stop(); e.preventDefault(); }}); addEventListener('blur',()=>{direction='';shiftHeld=false;stop()}); setInterval(()=>{if(shiftHeld&&direction)send('motion:hold')},100); connect();
</script></body></html>
)HTML";

void driversEnabled(bool enabled) { digitalWrite(DRIVER_ENABLE_PIN, enabled ? LOW : HIGH); }

void sendStatus(uint8_t client, const String &prefix = "") {
  String message = prefix + " direction=" + String(commandedDirection) +
    " maxSpeed=" + String(maxSpeed, 1) + " steps/s";
  webSocket.sendTXT(client, message);
  Serial.println(message);
}

void stopMotion(const char *reason) {
  commandedDirection = 0;
  verticalStepper.stop();       // Decelerates smoothly from the current speed.
  Serial.printf("Stopping: %s\n", reason);
}

void startMotion(int8_t direction) {
  commandedDirection = direction;
  lastHoldMs = millis();
  driversEnabled(true);
  verticalStepper.moveTo(direction > 0 ? CONTINUOUS_TARGET_STEPS : -CONTINUOUS_TARGET_STEPS);
}

void onWebSocketEvent(uint8_t client, WStype_t type, uint8_t *payload, size_t length) {
  if (type == WStype_DISCONNECTED) { shiftEnableHeld = false; stopMotion("WebSocket disconnected"); return; }
  if (type != WStype_TEXT) return;

  String message;
  message.reserve(length);
  for (size_t i = 0; i < length; ++i) message += (char)payload[i];

  if (message == "enable:shift:down") shiftEnableHeld = true;
  else if (message == "enable:shift:up") { shiftEnableHeld = false; stopMotion("Shift released"); }
  else if (message == "motion:up" && shiftEnableHeld) startMotion(1);
  else if (message == "motion:down" && shiftEnableHeld) startMotion(-1);
  else if (message == "motion:hold" && commandedDirection != 0 && shiftEnableHeld) lastHoldMs = millis();
  else if (message == "motion:stop") stopMotion("key released");
  else if (message.startsWith("speed:")) {
    float requested = message.substring(6).toFloat();
    if (requested >= MIN_SPEED_STEPS_PER_SECOND && requested <= MAX_ALLOWED_SPEED_STEPS_PER_SECOND) {
      maxSpeed = requested;
      verticalStepper.setMaxSpeed(maxSpeed);
      sendStatus(client, "Speed updated.");
      return;
    }
    webSocket.sendTXT(client, "Invalid speed. Use " + String(MIN_SPEED_STEPS_PER_SECOND, 0) + " to " + String(MAX_ALLOWED_SPEED_STEPS_PER_SECOND, 0) + " steps/s.");
    return;
  }
  sendStatus(client, "ESP32 received " + message + ".");
}

void setup() {
  Serial.begin(115200);
  pinMode(DRIVER_ENABLE_PIN, OUTPUT);
  driversEnabled(false); // Safe reset/startup state.

  verticalStepper.setPinsInverted(!VERTICAL_DIR_UP_LEVEL, false, false);
  verticalStepper.setMaxSpeed(maxSpeed);
  verticalStepper.setAcceleration(ACCELERATION_STEPS_PER_SECOND2);

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

  if (commandedDirection != 0 && millis() - lastHoldMs > DEADMAN_TIMEOUT_MS) stopMotion("dead-man timeout");
  verticalStepper.run();

  // Only disable after AccelStepper has decelerated fully to a stop.
  if (commandedDirection == 0 && verticalStepper.distanceToGo() == 0 && verticalStepper.speed() == 0) driversEnabled(false);
}
