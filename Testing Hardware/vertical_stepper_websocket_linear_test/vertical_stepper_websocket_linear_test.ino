/*
  vertical_stepper_websocket_linear_test.ino

  Fixed-speed vertical-stepper test. This version does not use AccelStepper:
  it generates STEP pulses directly and starts/stops without acceleration.

  Hold Shift + U = up. Hold Shift + D = down. Release either key = stop.
  M changes speed in steps/second. A 1000 ms heartbeat timeout stops and
  disables the drivers if browser communication is lost.

  WARNING: There are no limit switches in this test. Keep clear of the machine
  and be ready to remove motor power before it reaches a hard stop.
*/

#include <WiFi.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <ESPmDNS.h>

constexpr char MDNS_HOSTNAME[] = "cnc-press-brake";

const char *WIFI_SSID = "Rhymes with Donna";
const char *WIFI_PASSWORD = "!Bbrosgaming2020";

constexpr uint8_t VERTICAL_STEP_PIN = 25;
constexpr uint8_t VERTICAL_DIR_PIN = 26;
constexpr uint8_t DRIVER_ENABLE_PIN = 27;
constexpr bool VERTICAL_DIR_UP_LEVEL = HIGH;
constexpr float MIN_SPEED_STEPS_PER_SECOND = 10.0f;
constexpr float MAX_ALLOWED_SPEED_STEPS_PER_SECOND = 5000.0f;
constexpr uint32_t STEP_PULSE_WIDTH_US = 2;
constexpr unsigned long DEADMAN_TIMEOUT_MS = 1000;
constexpr unsigned long STATUS_UPDATE_INTERVAL_MS = 500;

WebServer httpServer(80);
WebSocketsServer webSocket(81);

float stepSpeed = 400.0f;
uint32_t stepPeriodUs = 2500;
int8_t commandedDirection = 0;
bool shiftEnableHeld = false;
bool stepPulseHigh = false;
uint32_t nextStepRiseUs = 0;
uint32_t pulseLowDueUs = 0;
unsigned long lastHoldMs = 0;
unsigned long lastStatusMs = 0;

const char CONTROL_PAGE[] PROGMEM = R"HTML(
<!doctype html><html><head><meta name="viewport" content="width=device-width,initial-scale=1">
<style>body{font:18px sans-serif;max-width:46rem;margin:2rem auto;padding:0 1rem}kbd{border:1px solid #777;padding:.2rem .5rem;border-radius:.25rem}.panel{padding:1rem;margin:.7rem 0;border-radius:.4rem;background:#eee;white-space:pre-wrap}.moving{color:#075;background:#dff7ea;font-weight:bold}.stopped{color:#700;background:#f5dddd}.pulse{display:inline-block;width:.8rem;height:.8rem;border-radius:50%;background:#888;margin-right:.5rem}.moving .pulse{background:#0a5;animation:pulse .6s infinite alternate}@keyframes pulse{to{transform:scale(1.5);opacity:.45}}</style>
</head><body><h1>Vertical stage fixed-speed test</h1>
<p>This version generates STEP pulses directly—no AccelStepper and no acceleration. Click here first. Hold <kbd>Shift</kbd> + <kbd>U</kbd> for up, or <kbd>Shift</kbd> + <kbd>D</kbd> for down. Releasing either key stops. <kbd>M</kbd> changes speed.</p>
<div id="request" class="panel stopped"><span class="pulse"></span>Browser request: STOPPED</div>
<div id="state" class="panel stopped"><span class="pulse"></span>ESP32: Connecting…</div><script>
let ws, direction = '', shiftHeld = false; const state = document.getElementById('state'), request = document.getElementById('request');
function send(m) { if (ws && ws.readyState === WebSocket.OPEN) ws.send(m); }
function stop() { send('motion:stop'); }
function panel(el,text,moving) { el.className='panel '+(moving?'moving':'stopped'); el.innerHTML='<span class="pulse"></span>'+text; }
function showRequest() { const moving=shiftHeld&&direction; panel(request,'Browser request: '+(moving?'MOVING '+direction.toUpperCase():'STOPPED'),moving); }
function updateMotion() { showRequest(); if (shiftHeld && direction) send('motion:'+direction); else stop(); }
function connect() { ws = new WebSocket(`ws://${location.hostname}:81/`); ws.onopen=()=>panel(state,'ESP32: Connected — waiting for motion command.',false); ws.onmessage=e=>{const moving=e.data.includes('direction=1')||e.data.includes('direction=-1');panel(state,'ESP32: '+e.data,moving)}; ws.onclose=()=>{panel(state,'ESP32: Disconnected — drivers should be disabled. Retrying…',false);setTimeout(connect,1000)}; }
addEventListener('keydown', e => { const k=e.key.toLowerCase(); if(k==='shift'&&!shiftHeld) { shiftHeld=true; send('enable:shift:down'); updateMotion(); e.preventDefault(); } else if((k==='u'||k==='d')&&direction!==(k==='u'?'up':'down')) { direction=k==='u'?'up':'down'; updateMotion(); e.preventDefault(); } else if(k==='m'&&!e.repeat) { const v=prompt('Fixed speed (steps/second):'); if(v!==null) send('speed:'+v); e.preventDefault(); }});
addEventListener('keyup', e => { const k=e.key.toLowerCase(); if(k==='shift') { shiftHeld=false; send('enable:shift:up'); stop(); showRequest(); e.preventDefault(); } else if((k==='u'&&direction==='up')||(k==='d'&&direction==='down')) { direction=''; stop(); showRequest(); e.preventDefault(); }});
addEventListener('blur',()=>{direction='';shiftHeld=false;stop();showRequest()});
setInterval(()=>{if(shiftHeld&&direction)send('motion:hold:'+direction)},100); connect();
</script></body></html>
)HTML";

void driversEnabled(bool enabled) {
  digitalWrite(DRIVER_ENABLE_PIN, enabled ? LOW : HIGH);
}

void updateStepPeriod() {
  stepPeriodUs = (uint32_t)(1000000.0f / stepSpeed);
}

void sendStatus(uint8_t client, const String &prefix = "") {
  String message = prefix + " direction=" + String(commandedDirection) +
    " fixedSpeed=" + String(stepSpeed, 1) + " steps/s";
  webSocket.sendTXT(client, message);
  Serial.println(message);
}

void stopMotion(const char *reason) {
  commandedDirection = 0;
  stepPulseHigh = false;
  digitalWrite(VERTICAL_STEP_PIN, LOW);
  driversEnabled(false);
  Serial.printf("Stopping immediately: %s\n", reason);
}

void startMotion(int8_t direction) {
  commandedDirection = direction;
  lastHoldMs = millis();
  digitalWrite(VERTICAL_DIR_PIN,
    direction > 0 ? VERTICAL_DIR_UP_LEVEL : !VERTICAL_DIR_UP_LEVEL);
  digitalWrite(VERTICAL_STEP_PIN, LOW);
  stepPulseHigh = false;
  nextStepRiseUs = micros() + 5; // Give DIR time to settle before the first STEP.
  driversEnabled(true);
}

void serviceStepPulses() {
  if (commandedDirection == 0) return;

  uint32_t nowUs = micros();
  if (stepPulseHigh) {
    if ((int32_t)(nowUs - pulseLowDueUs) >= 0) {
      digitalWrite(VERTICAL_STEP_PIN, LOW);
      stepPulseHigh = false;
    }
    return;
  }

  if ((int32_t)(nowUs - nextStepRiseUs) >= 0) {
    digitalWrite(VERTICAL_STEP_PIN, HIGH);
    stepPulseHigh = true;
    pulseLowDueUs = nowUs + STEP_PULSE_WIDTH_US;
    nextStepRiseUs += stepPeriodUs;

    // If other work delayed the loop by over one period, resume from now instead
    // of emitting a burst of catch-up pulses.
    if ((int32_t)(nowUs - nextStepRiseUs) >= 0) nextStepRiseUs = nowUs + stepPeriodUs;
  }
}

void onWebSocketEvent(uint8_t client, WStype_t type, uint8_t *payload, size_t length) {
  if (type == WStype_DISCONNECTED) {
    shiftEnableHeld = false;
    stopMotion("WebSocket disconnected");
    return;
  }
  if (type != WStype_TEXT) return;

  String message;
  message.reserve(length);
  for (size_t i = 0; i < length; ++i) message += (char)payload[i];

  if (message == "enable:shift:down") shiftEnableHeld = true;
  else if (message == "enable:shift:up") {
    shiftEnableHeld = false;
    stopMotion("Shift released");
  }
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
    return;
  }
  else if (message == "motion:stop") stopMotion("key released");
  else if (message.startsWith("speed:")) {
    float requested = message.substring(6).toFloat();
    if (requested >= MIN_SPEED_STEPS_PER_SECOND && requested <= MAX_ALLOWED_SPEED_STEPS_PER_SECOND) {
      stepSpeed = requested;
      updateStepPeriod();
      sendStatus(client, "Speed updated.");
      return;
    }
    webSocket.sendTXT(client, "Invalid speed. Use " +
      String(MIN_SPEED_STEPS_PER_SECOND, 0) + " to " +
      String(MAX_ALLOWED_SPEED_STEPS_PER_SECOND, 0) + " steps/s.");
    return;
  }
  sendStatus(client, "ESP32 received " + message + ".");
}

void setup() {
  Serial.begin(115200);
  pinMode(VERTICAL_STEP_PIN, OUTPUT);
  pinMode(VERTICAL_DIR_PIN, OUTPUT);
  pinMode(DRIVER_ENABLE_PIN, OUTPUT);
  digitalWrite(VERTICAL_STEP_PIN, LOW);
  driversEnabled(false);
  updateStepPeriod();

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting to Wi-Fi");
  while (WiFi.status() != WL_CONNECTED) { delay(250); Serial.print('.'); }
  if (MDNS.begin(MDNS_HOSTNAME)) {
    MDNS.addService("http", "tcp", 80);
    Serial.printf("\nOpen http://%s.local in a browser\n", MDNS_HOSTNAME);
  } else {
    Serial.printf("\nmDNS failed; open http://%s in a browser\n",
      WiFi.localIP().toString().c_str());
  }

  httpServer.on("/", []() {
    httpServer.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate");
    httpServer.send_P(200, "text/html", CONTROL_PAGE);
  });
  httpServer.begin();
  webSocket.begin();
  webSocket.onEvent(onWebSocketEvent);
}

void loop() {
  serviceStepPulses();
  httpServer.handleClient();
  serviceStepPulses();
  webSocket.loop();
  serviceStepPulses();

  if (commandedDirection != 0 && millis() - lastHoldMs > DEADMAN_TIMEOUT_MS) {
    stopMotion("dead-man timeout");
  }
}
