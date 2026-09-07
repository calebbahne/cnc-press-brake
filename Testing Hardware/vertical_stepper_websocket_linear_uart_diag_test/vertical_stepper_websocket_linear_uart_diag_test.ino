/*
  vertical_stepper_websocket_linear_uart_diag_test.ino

  Successor to vertical_stepper_websocket_linear_test.  It retains the direct,
  fixed-speed WebSocket pulse generator, but now uses the vertical TMC2209 UART
  bus and all installed vertical safety inputs.

  Required library: TMCStepper by teemuatlut.

  Wiring (electrical-quick-reference.md):
    GPIO25/14 STEP -> vertical TMC 1/2; GPIO26 -> both DIR; GPIO27 -> ENN
    GPIO17 -> 1k -> shared TMC RX/PDN_UART; GPIO16 -> same shared UART bus
    GPIO39 <- V1 DIAG; GPIO19 <- V2 DIAG
    GPIO34/35 mechanical limits are not used by this test.

  On the TMC2209, DIAG is the active-HIGH StallGuard output; it does not need
  software routing. A high DIAG, WebSocket loss, or dead-man timeout stops STEP
  immediately and disables the shared EN line. This bench test has no mechanical
  limit-switch protection, so keep the motor disconnected from dangerous motion.
*/

#include <WiFi.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <ESPmDNS.h>
#include <TMCStepper.h>

constexpr char MDNS_HOSTNAME[] = "cnc-press-brake";
const char *WIFI_SSID = "Rhymes with Donna";
const char *WIFI_PASSWORD = "!Bbrosgaming2020";

constexpr uint8_t STEP_1_PIN = 25, STEP_2_PIN = 14, DIR_PIN = 26, EN_PIN = 27;
constexpr uint8_t UART_RX_PIN = 16, UART_TX_PIN = 17;
constexpr uint8_t DIAG_1_PIN = 39, DIAG_2_PIN = 19;
constexpr uint8_t LIMIT_1_PIN = 34, LIMIT_2_PIN = 35;
constexpr bool UP_DIR_LEVEL = HIGH;
constexpr float MIN_SPEED = 10.0f, MAX_SPEED = 2000.0f;
constexpr uint32_t PULSE_US = 2, DEADMAN_MS = 1000, STATUS_MS = 500;
constexpr uint32_t DIAG_STARTUP_GRACE_MS = 150;
constexpr float R_SENSE = 0.110f;
constexpr uint16_t RUN_CURRENT_MA = 600; // Confirm against motor rating/cooling.
constexpr uint8_t SG_THRESHOLD = 0;      // Tune upward only after observing SG.
// About a 120 STEP/s lower StallGuard boundary with the nominal 12 MHz clock.
// Unlike 0xFFFFF, this keeps DIAG disabled at standstill.
constexpr uint32_t STALLGUARD_TCOOLTHRS = 100000;
// Set true after vertical driver 2 and its DIAG wire are installed.
constexpr bool VERTICAL_2_INSTALLED = false;
constexpr bool USE_VERTICAL_LIMITS = false;

HardwareSerial TMCSerial(2);
TMC2209Stepper tmc1(&TMCSerial, R_SENSE, 0);
TMC2209Stepper tmc2(&TMCSerial, R_SENSE, 1);
WebServer httpServer(80);
WebSocketsServer webSocket(81);

float speedSteps = 400.0f;
uint32_t periodUs = 2500, nextRiseUs = 0, lowDueUs = 0;
uint32_t lastHoldMs = 0, lastStatusMs = 0, motionStartedMs = 0;
int8_t direction = 0;
bool shiftHeld = false, pulseHigh = false, faultLatched = false;
bool uart1Ok = false, uart2Ok = false;
uint8_t uart1Result = 0, uart2Result = 0;
String faultReason;

const char CONTROL_PAGE[] PROGMEM = R"HTML(
<!doctype html><meta name="viewport" content="width=device-width,initial-scale=1"><style>body{font:18px sans-serif;max-width:46rem;margin:2rem auto;padding:0 1rem}.p{white-space:pre-wrap;padding:1rem;margin:.7rem 0;background:#eee;border-radius:.4rem}.bad{background:#f5dddd;color:#700}.ok{background:#dff7ea;color:#075}</style><h1>Vertical UART + DIAG test</h1><p>Hold <b>Shift</b> to energize. With Shift held, <b>U</b> moves up and <b>D</b> down. <b>M</b> sets speed. DIAG faults disable the drivers. This bench test has no limit switches.</p><div id="s" class="p">Connecting…</div><script>let ws,dir='',shift=0,s=document.querySelector('#s');function send(x){if(ws?.readyState===1)ws.send(x)}function state(x){s.textContent=x;s.className='p '+(/FAULT|disconnected/i.test(x)?'bad':'ok')}function go(){send(shift&&dir?'motion:'+dir:'motion:stop')}function con(){ws=new WebSocket(`ws://${location.hostname}:81/`);ws.onopen=()=>state('Connected — waiting.');ws.onmessage=e=>state(e.data);ws.onclose=()=>{state('Disconnected — retrying.');setTimeout(con,1000)}}addEventListener('keydown',e=>{let k=e.key.toLowerCase();if(k==='shift'&&!shift){shift=1;send('enable:on');go()}else if(k==='u'||k==='d'){dir=k==='u'?'up':'down';go()}else if(k==='m'&&!e.repeat){let v=prompt('Speed, steps/second');if(v!==null)send('speed:'+v)}e.preventDefault()});addEventListener('keyup',e=>{let k=e.key.toLowerCase();if(k==='shift'){shift=0;dir='';send('enable:off')}else if((k==='u'&&dir==='up')||(k==='d'&&dir==='down')){dir='';go()}e.preventDefault()});addEventListener('blur',()=>{shift=0;dir='';send('enable:off')});setInterval(()=>{if(shift)send('hold')},100);con()</script>
)HTML";

void setEnabled(bool enabled) { digitalWrite(EN_PIN, enabled ? LOW : HIGH); }
bool limitOpen() {
  return USE_VERTICAL_LIMITS &&
         (digitalRead(LIMIT_1_PIN) ||
          (VERTICAL_2_INSTALLED && digitalRead(LIMIT_2_PIN)));
}
bool diagActive() {
  return digitalRead(DIAG_1_PIN) ||
         (VERTICAL_2_INSTALLED && digitalRead(DIAG_2_PIN));
}

void updatePeriod() { periodUs = uint32_t(1000000.0f / speedSteps); }

void stopAndDisable(const String &reason, bool latch) {
  direction = 0; pulseHigh = false;
  digitalWrite(STEP_1_PIN, LOW); digitalWrite(STEP_2_PIN, LOW);
  setEnabled(false);
  if (latch) { faultLatched = true; faultReason = reason; }
  Serial.println("STOP: " + reason);
}

void report(uint8_t client, const String &prefix = "") {
  String uart1 = uart1Ok ? "OK" : "FAILED(" + String(uart1Result) + ")";
  String uart2 = !VERTICAL_2_INSTALLED ? "NA" :
                 (uart2Ok ? "OK" : "FAILED(" + String(uart2Result) + ")");
  String sg1 = uart1Ok ? String(tmc1.SG_RESULT()) : "NA";
  String sg2 = VERTICAL_2_INSTALLED && uart2Ok ? String(tmc2.SG_RESULT()) : "NA";
  String diag2 = VERTICAL_2_INSTALLED ? String(digitalRead(DIAG_2_PIN)) : "NA";
  String limit1 = USE_VERTICAL_LIMITS ? String(digitalRead(LIMIT_1_PIN)) : "NA";
  String limit2 = USE_VERTICAL_LIMITS && VERTICAL_2_INSTALLED ? String(digitalRead(LIMIT_2_PIN)) : "NA";
  String msg = prefix + " dir=" + String(direction) + " EN=" +
    String(shiftHeld && !faultLatched ? "on" : "off") + " speed=" +
    String(speedSteps, 0) + " UART=" + uart1 + "/" + uart2 +
    " SG=" + sg1 + "/" + sg2 +
    " DIAG=" + String(digitalRead(DIAG_1_PIN)) + "/" + diag2 +
    " limit=" + limit1 + "/" + limit2;
  if (faultLatched) msg += " FAULT: " + faultReason;
  webSocket.sendTXT(client, msg); Serial.println(msg);
}

void startMotion(int8_t requested) {
  if (faultLatched || limitOpen()) return;
  direction = requested;
  motionStartedMs = millis();
  digitalWrite(DIR_PIN, requested > 0 ? UP_DIR_LEVEL : !UP_DIR_LEVEL);
  digitalWrite(STEP_1_PIN, LOW);
  if (VERTICAL_2_INSTALLED) digitalWrite(STEP_2_PIN, LOW);
  pulseHigh = false; nextRiseUs = micros() + 5; setEnabled(true);
}

void servicePulses() {
  if (!direction) return;
  uint32_t now = micros();
  if (pulseHigh) { if (int32_t(now - lowDueUs) >= 0) { digitalWrite(STEP_1_PIN, LOW); if (VERTICAL_2_INSTALLED) digitalWrite(STEP_2_PIN, LOW); pulseHigh = false; } return; }
  if (int32_t(now - nextRiseUs) >= 0) {
    digitalWrite(STEP_1_PIN, HIGH);
    if (VERTICAL_2_INSTALLED) digitalWrite(STEP_2_PIN, HIGH);
    pulseHigh = true; lowDueUs = now + PULSE_US; nextRiseUs += periodUs;
    if (int32_t(now - nextRiseUs) >= 0) nextRiseUs = now + periodUs;
  }
}

void checkSafety() {
  if (faultLatched) return;
  if (limitOpen()) stopAndDisable("NC vertical limit open", true);
  else if (direction != 0 && millis() - motionStartedMs >= DIAG_STARTUP_GRACE_MS && diagActive())
    stopAndDisable("TMC DIAG asserted while moving", true);
}

void configure(TMC2209Stepper &tmc) {
  tmc.begin(); tmc.toff(4); tmc.blank_time(24); tmc.microsteps(16); tmc.intpol(true);
  tmc.en_spreadCycle(false); tmc.pwm_autoscale(true); tmc.I_scale_analog(false);
  tmc.rms_current(RUN_CURRENT_MA, 0.5f);
  tmc.TCOOLTHRS(STALLGUARD_TCOOLTHRS);
  tmc.SGTHRS(SG_THRESHOLD);
  // Unlike several other Trinamic parts, TMC2209Stepper does not expose
  // diag0_* routing methods. The TMC2209's DIAG output natively reports the
  // StallGuard comparison configured by TCOOLTHRS and SGTHRS above.
  tmc.GSTAT(0b111);
}

void websocketEvent(uint8_t client, WStype_t type, uint8_t *payload, size_t length) {
  if (type == WStype_DISCONNECTED) { shiftHeld = false; stopAndDisable("WebSocket disconnected", false); return; }
  if (type != WStype_TEXT) return;
  String m; for (size_t i = 0; i < length; ++i) m += char(payload[i]);
  if (m == "enable:off") { shiftHeld = false; stopAndDisable("Shift released", false); }
  else if (m == "enable:on") {
    if (limitOpen()) stopAndDisable("cannot enable: active limit", true);
    else { faultLatched = false; faultReason = ""; shiftHeld = true; lastHoldMs = millis(); setEnabled(true); }
  } else if (m == "hold" && shiftHeld) lastHoldMs = millis();
  else if (m == "motion:up" && shiftHeld) startMotion(1);
  else if (m == "motion:down" && shiftHeld) startMotion(-1);
  else if (m == "motion:stop") { direction = 0; digitalWrite(STEP_1_PIN, LOW); digitalWrite(STEP_2_PIN, LOW); }
  else if (m.startsWith("speed:")) { float v = m.substring(6).toFloat(); if (v >= MIN_SPEED && v <= MAX_SPEED) { speedSteps = v; updatePeriod(); } }
  report(client, "ESP32");
}

void setup() {
  Serial.begin(115200);
  pinMode(STEP_1_PIN, OUTPUT); pinMode(STEP_2_PIN, OUTPUT); pinMode(DIR_PIN, OUTPUT); pinMode(EN_PIN, OUTPUT);
  pinMode(DIAG_1_PIN, INPUT); pinMode(DIAG_2_PIN, INPUT); pinMode(LIMIT_1_PIN, INPUT); pinMode(LIMIT_2_PIN, INPUT);
  digitalWrite(STEP_1_PIN, LOW); digitalWrite(STEP_2_PIN, LOW); setEnabled(false); updatePeriod();
  TMCSerial.begin(115200, SERIAL_8N1, UART_RX_PIN, UART_TX_PIN);
  configure(tmc1);
  if (VERTICAL_2_INSTALLED) configure(tmc2);
  uart1Result = tmc1.test_connection();
  uart1Ok = uart1Result == 0;
  if (VERTICAL_2_INSTALLED) uart2Result = tmc2.test_connection();
  uart2Ok = VERTICAL_2_INSTALLED && uart2Result == 0;
  Serial.printf("TMC UART V1=%s V2=%s\n",
                uart1Ok ? "OK" : (uart1Result == 1 ? "FAILED (bad/no reply)" : "FAILED (all-zero reply)"),
                VERTICAL_2_INSTALLED ? (uart2Ok ? "OK" : "FAILED") : "NOT INSTALLED");
  checkSafety();
  WiFi.mode(WIFI_STA); WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) delay(250);
  MDNS.begin(MDNS_HOSTNAME); MDNS.addService("http", "tcp", 80);
  httpServer.on("/", [] { httpServer.sendHeader("Cache-Control", "no-store"); httpServer.send_P(200, "text/html", CONTROL_PAGE); });
  httpServer.begin(); webSocket.begin(); webSocket.onEvent(websocketEvent);
  Serial.printf("Open http://%s.local\n", MDNS_HOSTNAME);
}

void loop() {
  checkSafety(); servicePulses(); httpServer.handleClient(); servicePulses(); webSocket.loop(); servicePulses();
  if (shiftHeld && millis() - lastHoldMs > DEADMAN_MS) { shiftHeld = false; stopAndDisable("dead-man timeout", false); }
  if (millis() - lastStatusMs >= STATUS_MS) { lastStatusMs = millis(); if (webSocket.connectedClients()) report(0); }
}
