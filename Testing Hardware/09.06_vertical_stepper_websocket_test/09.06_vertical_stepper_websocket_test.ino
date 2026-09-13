/*
  vertical_stepper_websocket_test.ino

  PURPOSE: Manual vertical-stepper test only. It controls both vertical STEP
  signals (GPIO 25 and GPIO 14), their shared DIR signal (GPIO 26), and the
  active-low global enable (GPIO 27). Both vertical motors receive the same
  motion command, while retaining independent STEP wiring for later homing.

  BEFORE POWERING MOTORS:
  - Set WIFI_SSID, WIFI_PASSWORD, and confirm VERTICAL_DIR_UP_LEVEL. Start at
    a low MAX_SPEED_STEPS_PER_SECOND and verify which way "up" actually moves.
  - This is a bring-up test with no limit switches, DIAG, or TMC UART connected.
    Keep clear of the machine, be ready to remove motor power, and do not use it
    on a machine that can contact tooling or reach a mechanical hard stop.
  - Upload, open Serial Monitor (115200), then browse to the printed IP address.
    Click the page to give it keyboard focus.

  CONTROLS:
    Hold Shift = energize drivers and hold the motor shafts rigid.
    Shift + U = up. Release U = stop while remaining energized.
    Shift + D = down. Release D = stop while remaining energized.
    M = enter a new maximum speed in the browser prompt (steps/second).
    H = open the on-screen help.

  A 1000 ms WebSocket dead-man timeout disables the drivers if communication or
  the browser stops sending heartbeats. Required libraries: AccelStepper and
  WebSockets by Markus Sattler; WiFi/WebServer come with ESP32 boards.
*/

#include <WiFi.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <AccelStepper.h>
#include <ESPmDNS.h>

constexpr char MDNS_HOSTNAME[] = "cnc-press-brake";

const char *WIFI_SSID = "Rhymes with Donna";
const char *WIFI_PASSWORD = "!Bbrosgaming2020";

constexpr uint8_t VERTICAL_MOTOR_1_STEP_PIN = 25;
constexpr uint8_t VERTICAL_MOTOR_2_STEP_PIN = 14;
constexpr uint8_t VERTICAL_DIR_PIN = 26;
constexpr uint8_t DRIVER_ENABLE_PIN = 27;  // TMC2209 EN is active-low.
constexpr bool VERTICAL_DIR_UP_LEVEL = HIGH; // Change to LOW if U moves down.
constexpr float ACCELERATION_STEPS_PER_SECOND2 = 800.0f;
constexpr float MIN_SPEED_STEPS_PER_SECOND = 10.0f;
constexpr float MAX_ALLOWED_SPEED_STEPS_PER_SECOND = 5000.0f;
constexpr unsigned long DEADMAN_TIMEOUT_MS = 1000;
constexpr unsigned long STATUS_UPDATE_INTERVAL_MS = 500;
constexpr long CONTINUOUS_TARGET_STEPS = 100000000L;

WebServer httpServer(80);
WebSocketsServer webSocket(81);
AccelStepper verticalMotor1(AccelStepper::DRIVER, VERTICAL_MOTOR_1_STEP_PIN, VERTICAL_DIR_PIN);
AccelStepper verticalMotor2(AccelStepper::DRIVER, VERTICAL_MOTOR_2_STEP_PIN, VERTICAL_DIR_PIN);

float maxSpeed = 400.0f;
int8_t commandedDirection = 0; // +1 up, -1 down, 0 stopped
bool shiftEnableHeld = false;
unsigned long lastHoldMs = 0;
unsigned long lastStatusMs = 0;

const char CONTROL_PAGE[] PROGMEM = R"HTML(
<!doctype html><html><head><meta name="viewport" content="width=device-width,initial-scale=1">
<style>body{font:18px sans-serif;max-width:46rem;margin:2rem auto;padding:0 1rem}kbd{border:1px solid #777;padding:.2rem .5rem;border-radius:.25rem}.panel{padding:1rem;margin:.7rem 0;border-radius:.4rem;background:#eee;white-space:pre-wrap}.moving{color:#075;background:#dff7ea;font-weight:bold}.stopped{color:#700;background:#f5dddd}.pulse{display:inline-block;width:.8rem;height:.8rem;border-radius:50%;background:#888;margin-right:.5rem}.moving .pulse{background:#0a5;animation:pulse .6s infinite alternate}@keyframes pulse{to{transform:scale(1.5);opacity:.45}}</style>
</head><body><h1>Vertical stage stepper test</h1>
<p>Click here first. Hold <kbd>Shift</kbd> to energize the motors and hold the shafts rigid. While holding Shift, press <kbd>U</kbd> for up or <kbd>D</kbd> for down. Releasing the direction key stops motion but maintains holding torque. <kbd>M</kbd> changes maximum speed; <kbd>H</kbd> shows help.</p>
<div id="request" class="panel stopped"><span class="pulse"></span>Browser request: STOPPED</div>
<div id="state" class="panel stopped"><span class="pulse"></span>ESP32: Connecting…</div><script>
let ws, direction = '', shiftHeld = false; const state = document.getElementById('state'), request = document.getElementById('request');
function send(m) { if (ws && ws.readyState === WebSocket.OPEN) ws.send(m); }
function stop() { send('motion:stop'); }
function panel(el,text,moving) { el.className='panel '+(moving?'moving':'stopped'); el.innerHTML='<span class="pulse"></span>'+text; }
function showRequest() { const moving=shiftHeld&&direction; panel(request,'Browser request: '+(moving?'MOVING '+direction.toUpperCase():'STOPPED'),moving); }
function updateMotion() { showRequest(); if (shiftHeld && direction) send('motion:'+direction); else stop(); }
function connect() { ws = new WebSocket(`ws://${location.hostname}:81/`); ws.onopen=()=>panel(state,'ESP32: Connected — waiting for motion command.',false); ws.onmessage=e=>{const moving=e.data.includes('direction=1')||e.data.includes('direction=-1');panel(state,'ESP32: '+e.data,moving)}; ws.onclose=()=>{panel(state,'ESP32: Disconnected — drivers should be disabled. Retrying…',false);setTimeout(connect,1000)}; }
addEventListener('keydown', e => { const k=e.key.toLowerCase(); if(k==='shift'&&!shiftHeld) { shiftHeld=true; send('enable:shift:down'); updateMotion(); e.preventDefault(); } else if((k==='u'||k==='d')&&direction!==(k==='u'?'up':'down')) { direction=k==='u'?'up':'down'; updateMotion(); e.preventDefault(); } else if(k==='m'&&!e.repeat) { const v=prompt('Maximum speed (steps/second):'); if(v!==null) send('speed:'+v); e.preventDefault(); } else if(k==='h'&&!e.repeat) { alert('Hold Shift: energize motors and hold position\nShift + U: move up\nShift + D: move down\nRelease U or D: stop with holding torque\nRelease Shift: disable motors\nM: change maximum speed\nH: this help'); e.preventDefault(); }});
addEventListener('keyup', e => { const k=e.key.toLowerCase(); if(k==='shift') { shiftHeld=false; send('enable:shift:up'); stop(); showRequest(); e.preventDefault(); } else if((k==='u'&&direction==='up')||(k==='d'&&direction==='down')) { direction=''; stop(); showRequest(); e.preventDefault(); }}); addEventListener('blur',()=>{direction='';shiftHeld=false;stop();showRequest()}); setInterval(()=>{if(shiftHeld)send(direction?'motion:hold:'+direction:'enable:shift:hold')},100); connect();
</script></body></html>
)HTML";

void driversEnabled(bool enabled) { digitalWrite(DRIVER_ENABLE_PIN, enabled ? LOW : HIGH); }

void sendStatus(uint8_t client, const String &prefix = "") {
  String message = prefix + " direction=" + String(commandedDirection) +
    " drivers=" + String(shiftEnableHeld ? "enabled" : "disabled") +
    " maxSpeed=" + String(maxSpeed, 1) + " steps/s";
  webSocket.sendTXT(client, message);
  Serial.println(message);
}

void stopMotion(const char *reason) {
  commandedDirection = 0;
  verticalMotor1.stop();        // Decelerates smoothly from the current speed.
  verticalMotor2.stop();
  Serial.printf("Stopping: %s\n", reason);
}

void startMotion(int8_t direction) {
  commandedDirection = direction;
  lastHoldMs = millis();
  driversEnabled(true);
  long target = direction > 0 ? CONTINUOUS_TARGET_STEPS : -CONTINUOUS_TARGET_STEPS;
  verticalMotor1.moveTo(target);
  verticalMotor2.moveTo(target);
}

void onWebSocketEvent(uint8_t client, WStype_t type, uint8_t *payload, size_t length) {
  if (type == WStype_DISCONNECTED) { shiftEnableHeld = false; stopMotion("WebSocket disconnected"); return; }
  if (type != WStype_TEXT) return;

  String message;
  message.reserve(length);
  for (size_t i = 0; i < length; ++i) message += (char)payload[i];

  if (message == "enable:shift:down") {
    shiftEnableHeld = true;
    lastHoldMs = millis();
    driversEnabled(true);
  }
  else if (message == "enable:shift:hold" && shiftEnableHeld) {
    lastHoldMs = millis();
    return;
  }
  else if (message == "enable:shift:up") { shiftEnableHeld = false; stopMotion("Shift released"); }
  else if (message == "motion:up" && shiftEnableHeld) startMotion(1);
  else if (message == "motion:down" && shiftEnableHeld) startMotion(-1);
  else if ((message == "motion:hold:up" || message == "motion:hold:down") && shiftEnableHeld) {
    int8_t requestedDirection = message.endsWith("up") ? 1 : -1;
    if (commandedDirection != requestedDirection) startMotion(requestedDirection);
    else lastHoldMs = millis();
    if (millis() - lastStatusMs >= STATUS_UPDATE_INTERVAL_MS) {
      lastStatusMs = millis();
      sendStatus(client, "Heartbeat confirmed.");
    }
    return; // Avoid a WebSocket reply and Serial print for every 100 ms heartbeat.
  }
  else if (message == "motion:stop") stopMotion("key released");
  else if (message.startsWith("speed:")) {
    float requested = message.substring(6).toFloat();
    if (requested >= MIN_SPEED_STEPS_PER_SECOND && requested <= MAX_ALLOWED_SPEED_STEPS_PER_SECOND) {
      maxSpeed = requested;
      verticalMotor1.setMaxSpeed(maxSpeed);
      verticalMotor2.setMaxSpeed(maxSpeed);
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

  verticalMotor1.setPinsInverted(!VERTICAL_DIR_UP_LEVEL, false, false);
  verticalMotor2.setPinsInverted(!VERTICAL_DIR_UP_LEVEL, false, false);
  verticalMotor1.setMinPulseWidth(2);
  verticalMotor2.setMinPulseWidth(2);
  verticalMotor1.setMaxSpeed(maxSpeed);
  verticalMotor2.setMaxSpeed(maxSpeed);
  verticalMotor1.setAcceleration(ACCELERATION_STEPS_PER_SECOND2);
  verticalMotor2.setAcceleration(ACCELERATION_STEPS_PER_SECOND2);

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

  if (shiftEnableHeld && millis() - lastHoldMs > DEADMAN_TIMEOUT_MS) {
    shiftEnableHeld = false;
    stopMotion("dead-man timeout");
  }
  verticalMotor1.run();
  verticalMotor2.run();

  // Only disable after AccelStepper has decelerated fully to a stop.
  if (!shiftEnableHeld && commandedDirection == 0 &&
      verticalMotor1.distanceToGo() == 0 && verticalMotor1.speed() == 0 &&
      verticalMotor2.distanceToGo() == 0 && verticalMotor2.speed() == 0) {
    driversEnabled(false);
  }
}
