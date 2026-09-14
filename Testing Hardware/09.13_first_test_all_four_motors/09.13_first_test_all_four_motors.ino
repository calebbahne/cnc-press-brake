/* Four-motor commissioning test. See README.md and electrical-quick-reference.md.
   No limit switches or homing. Positions count commanded full steps only.
   Global EN energizes all physically connected drivers; support vertical loads.
*/
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <ESPmDNS.h>
#include <TMCStepper.h>
#include <esp_timer.h>
#include <math.h>
#include <errno.h>
#include "control_page.h"

// EDIT THESE FOUR SWITCHES BEFORE UPLOADING. False means physically unplugged.
// Never unplug drivers/motors under power. A false switch is NOT electrical isolation.
constexpr bool VERTICAL_1_CONNECTED = false;   // UART 0, STEP 25
constexpr bool VERTICAL_2_CONNECTED = false;   // UART 2, STEP 14
constexpr bool HORIZONTAL_1_CONNECTED = true; // UART 1, shared STEP 32
constexpr bool HORIZONTAL_2_CONNECTED = true; // UART 3, shared STEP 32
// Optional individual shaft inversion for mirrored motor installations.
constexpr bool INVERT_V1 = false, INVERT_V2 = false;
constexpr bool INVERT_H1 = false, INVERT_H2 = false;

// Time allowed without a browser heartbeat while outputs are armed.
// Increase this if brief Wi-Fi/browser delays cause watchdog faults.
constexpr uint32_t BROWSER_WATCHDOG_MS = 750;

const char *WIFI_SSID = "Rhymes with Donna";
const char *WIFI_PASSWORD = "!Bbrosgaming2020";
constexpr int EN = 27;
constexpr int DIR_PINS[2] = {26, 33};
constexpr int DIAG_PINS[4] = {39, 19, 18, 23};
constexpr bool PRESENT[4] = {VERTICAL_1_CONNECTED, VERTICAL_2_CONNECTED,
                            HORIZONTAL_1_CONNECTED, HORIZONTAL_2_CONNECTED};
constexpr bool INVERT[4] = {INVERT_V1, INVERT_V2, INVERT_H1, INVERT_H2};
constexpr int ADDRESS[4] = {0, 2, 1, 3};
const char *NAMES[4] = {"Vertical 1", "Vertical 2", "Horizontal 1", "Horizontal 2"};
constexpr int32_t POSITION_BOUND = 1000000000;

struct Settings {
  int current = 600, hold = 50, precharge = 400;
  int vSpeed = 250, vAccel = 100, hSpeed = 250, hAccel = 100, start = 20;
  int maxTravel = 600, sgthrs = 0, senseMin = 200, settle = 300;
  bool stopDiag = false;
} cfg;
HardwareSerial uart(2);
TMC2209Stepper v1(&uart, 0.110f, 0), v2(&uart, 0.110f, 2);
TMC2209Stepper h1(&uart, 0.110f, 1), h2(&uart, 0.110f, 3);
TMC2209Stepper *drivers[4] = {&v1, &v2, &h1, &h2};
WebServer http(80);
WebSocketsServer ws(81);
esp_timer_handle_t pulseTimer = nullptr;
portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
// Timer-shared state: always access under mux. UART/network only in loop().
bool enabled = false, sensing = false;
int activeAxis = -1, direction = 0, fault = 0;
int32_t position[2] = {0, 0}, remaining = 0;
bool zeroed[2] = {false, false};
float velocity = 0;
int64_t heartbeat = 0, prechargeUntil = 0, lastTick = 0, nextStep = 0, cruiseSince = 0;
uint32_t diagEdges[4] = {};
bool ready = false, seen[4] = {}, configured[4] = {};
uint32_t drv[4] = {}, gstat[4] = {}, sampleAt[4] = {};
uint16_t sg[4] = {};
int owner = -1, runHoldAxis = -1;
const char *faultText[] = {"none", "DIAG stall", "browser watchdog/disconnect",
  "UART/configuration lost", "driver temperature/short/undervoltage", "timer unavailable"};

void stepsLow() {
  digitalWrite(25, LOW); digitalWrite(14, LOW); digitalWrite(32, LOW);
}
void haltLocked(bool disable) {
  activeAxis = -1; direction = 0; velocity = 0; remaining = 0; sensing = false;
  cruiseSince = 0; stepsLow();
  if (disable) {
    enabled = false; digitalWrite(EN, HIGH);
    zeroed[0] = false; zeroed[1] = false;
  }
}
void stopMotion(bool disable) {
  portENTER_CRITICAL(&mux); haltLocked(disable); portEXIT_CRITICAL(&mux);
}
void latchFault(int code) {
  portENTER_CRITICAL(&mux);
  if (!fault) fault = code;
  zeroed[0] = false; zeroed[1] = false;
  haltLocked(code != 1);
  portEXIT_CRITICAL(&mux);
}
void IRAM_ATTR diagISR(void *arg) {
  const int i = static_cast<int>(reinterpret_cast<intptr_t>(arg));
  portENTER_CRITICAL_ISR(&mux);
  ++diagEdges[i];
  if (enabled && activeAxis == i / 2 && sensing && cfg.stopDiag && !fault) fault = 1;
  portEXIT_CRITICAL_ISR(&mux);
}
// No UART/network/heap or catch-up bursts in this 100 us timer callback.
void motionTick(void *) {
  const int64_t now = esp_timer_get_time();
  portENTER_CRITICAL(&mux);
  if (enabled && now - heartbeat > int64_t(BROWSER_WATCHDOG_MS) * 1000LL) {
    if (!fault) fault = 2;
    haltLocked(true);
  }
  if (fault) {
    zeroed[0] = false; zeroed[1] = false;
    haltLocked(fault != 1);
  }
  if (!enabled || activeAxis < 0 || now < prechargeUntil) {
    lastTick = now; portEXIT_CRITICAL(&mux); return;
  }
  const int a = activeAxis;
  const float maxSpeed = a == 0 ? cfg.vSpeed : cfg.hSpeed;
  const float accel = a == 0 ? cfg.vAccel : cfg.hAccel;
  const float dt = fminf((now - lastTick) / 1000000.0f, 0.002f);
  lastTick = now;
  // Ramp down toward start speed before the final pulse of a bounded move.
  const float cap = fminf(maxSpeed, sqrtf(float(cfg.start) * cfg.start + 2.0f * accel * (remaining - 1)));
  if (velocity < cap) velocity = fminf(cap, velocity + accel * dt);
  else velocity = fmaxf(cap, velocity - accel * dt);
  if (velocity >= maxSpeed) { if (!cruiseSince) cruiseSince = now; }
  else cruiseSince = 0;
  sensing = cruiseSince && now - cruiseSince >= cfg.settle * 1000LL && velocity >= cfg.senseMin;
  for (int i = a * 2; i < a * 2 + 2; ++i) {
    if (PRESENT[i] && cfg.stopDiag && sensing && digitalRead(DIAG_PINS[i])) fault = 1;
  }
  if (fault) {
    zeroed[0] = false; zeroed[1] = false; haltLocked(false);
    portEXIT_CRITICAL(&mux); return;
  }
  if (now >= nextStep) {
    // Both selected vertical motors receive each pulse; horizontal STEP is shared.
    if (a == 0) {
      if (PRESENT[0]) digitalWrite(25, HIGH);
      if (PRESENT[1]) digitalWrite(14, HIGH);
    } else digitalWrite(32, HIGH);
    delayMicroseconds(3); stepsLow();
    position[a] += direction;
    nextStep = now + static_cast<int64_t>(1000000.0f / velocity);
    if (--remaining == 0) haltLocked(false); // Reaching a target is normal completion.
  }
  portEXIT_CRITICAL(&mux);
}
bool configurationMatches(int i) {
  auto &d = *drivers[i];
  const uint32_t gc = d.GCONF(), cc = d.CHOPCONF(), pc = d.PWMCONF();
  return (gc & 0xCF) == (0xC0U | (INVERT[i] ? 8U : 0U)) &&
    ((cc >> 24) & 15) == 8 && !(cc & (1UL << 28)) &&
    (cc & 15) == 4 && (pc & (3UL << 18)) == (3UL << 18);
}
bool configureDriver(int i) {
  auto &d = *drivers[i];
  seen[i] = ((d.IOIN() >> 24) & 255) == 0x21 && d.test_connection() == 0;
  if (!seen[i]) return configured[i] = false;
  const uint8_t before = d.IFCNT();
  d.GSTAT(7);
  d.pdn_disable(true); d.mstep_reg_select(true);
  d.I_scale_analog(false); d.internal_Rsense(false); d.shaft(INVERT[i]);
  d.rms_current(cfg.current, cfg.hold / 100.0f);
  d.iholddelay(8); d.TPOWERDOWN(20);
  d.toff(4); d.blank_time(24);
  d.microsteps(0); d.intpol(false); d.dedge(false);
  d.en_spreadCycle(false); d.TPWMTHRS(0);
  d.pwm_autoscale(true); d.pwm_autograd(true);
  d.semin(0); d.SGTHRS(cfg.sgthrs);
  d.TCOOLTHRS(static_cast<uint32_t>(12000000.0f / (256.0f * cfg.senseMin)));
  d.VACTUAL(0);
  const bool acknowledged = static_cast<uint8_t>(d.IFCNT() - before) > 0;
  drv[i] = d.DRV_STATUS(); gstat[i] = d.GSTAT(); sampleAt[i] = millis();
  configured[i] = acknowledged && configurationMatches(i) &&
    drv[i] != 0xFFFFFFFF && !(drv[i] & 0x3F) && !(gstat[i] & 6);
  Serial.printf("%s UART %d: %s\n", NAMES[i], ADDRESS[i], configured[i] ? "CONFIG PASS" : "CONFIG FAIL");
  return configured[i];
}
bool configureAll() {
  bool ok = true, any = false;
  for (int i = 0; i < 4; ++i) {
    if (!PRESENT[i]) continue;
    any = true;
    if (!configureDriver(i)) ok = false;
  }
  runHoldAxis = -1;
  return any && ok;
}
bool setCurrentMode(int axis) {
  for (int i = 0; i < 4; ++i) if (PRESENT[i]) {
    auto &d = *drivers[i];
    const uint8_t before = d.IFCNT();
    d.rms_current(cfg.current, i / 2 == axis ? 1.0f : cfg.hold / 100.0f);
    if (static_cast<uint8_t>(d.IFCNT() - before) == 0) {
      configured[i] = false; ready = false; latchFault(3); return false;
    }
  }
  runHoldAxis = axis;
  return true;
}
// Poll one selected driver per interval to avoid four missing UART timeouts at once.
void pollDriver(int i) {
  auto &d = *drivers[i];
  seen[i] = ((d.IOIN() >> 24) & 255) == 0x21;
  if (!seen[i]) { configured[i] = false; ready = false; latchFault(3); return; }
  drv[i] = d.DRV_STATUS(); gstat[i] = d.GSTAT(); sg[i] = d.SG_RESULT();
  sampleAt[i] = millis();
  if (drv[i] == 0xFFFFFFFF || sg[i] > 510) {
    seen[i] = false; configured[i] = false; ready = false; latchFault(3); return;
  }
  if ((drv[i] & 0x3F) || (gstat[i] & 6)) latchFault(4);
  if (gstat[i] & 1) {
    if (!configurationMatches(i)) {
      configured[i] = false; ready = false; latchFault(3);
    } else d.GSTAT(1);
  }
}
bool parseNumber(const String &raw, long low, long high, long &value) {
  if (!raw.length()) return false;
  size_t n = raw[0] == '-' ? 1 : 0;
  if (n == raw.length()) return false;
  for (; n < raw.length(); ++n) if (raw[n] < '0' || raw[n] > '9') return false;
  errno = 0; char *end;
  value = strtol(raw.c_str(), &end, 10);
  return !errno && !*end && value >= low && value <= high;
}
void notice(uint8_t client, const char *message) {
  String s = String("{\"notice\":\"") + message + "\"}"; ws.sendTXT(client, s);
}
void startMove(uint8_t client, int axis, long amount, bool absolute) {
  portENTER_CRITICAL(&mux);
  const bool allowed = enabled && !fault && activeAxis < 0 && owner == client;
  const int32_t pos = position[axis];
  const bool hasZero = zeroed[axis];
  portEXIT_CRITICAL(&mux);
  if (!allowed) { notice(client, "Arm first; stop the current stage before another move."); return; }
  if (!PRESENT[axis * 2] && !PRESENT[axis * 2 + 1]) { notice(client, "No drivers selected for this stage."); return; }
  if (absolute && !hasZero) { notice(client, "Set this stage zero first."); return; }
  const int64_t delta = absolute ? int64_t(amount) - pos : amount;
  const int64_t target = int64_t(pos) + delta;
  if (delta > cfg.maxTravel || delta < -cfg.maxTravel || target > POSITION_BOUND || target < -POSITION_BOUND) {
    notice(client, "Move exceeds the per-command step budget or position range."); return;
  }
  if (!delta) { notice(client, "Already at target; no steps requested."); return; }
  if (!setCurrentMode(axis)) return;
  portENTER_CRITICAL(&mux);
  if (enabled && !fault && activeAxis < 0) {
    activeAxis = axis; direction = delta > 0 ? 1 : -1;
    remaining = static_cast<int32_t>(delta > 0 ? delta : -delta);
    digitalWrite(DIR_PINS[axis], direction > 0 ? HIGH : LOW);
    velocity = cfg.start; sensing = false; cruiseSince = 0;
    const int64_t now = esp_timer_get_time();
    prechargeUntil = now + cfg.precharge * 1000LL;
    nextStep = prechargeUntil; lastTick = now;
  }
  portEXIT_CRITICAL(&mux);
}
String settingsJSON() {
  String s = "{";
#define FIELD(name) s += "\"" #name "\":" + String(cfg.name) + ",";
  FIELD(current) FIELD(hold) FIELD(precharge)
  FIELD(vSpeed) FIELD(vAccel) FIELD(hSpeed) FIELD(hAccel) FIELD(start)
  FIELD(maxTravel) FIELD(sgthrs) FIELD(senseMin) FIELD(settle)
#undef FIELD
  return s + "\"stopDiag\":" + (cfg.stopDiag ? "true" : "false") + "}";
}
void sendTelemetry() {
  int a, f; bool e, z[2], window; int32_t p[2], left; float speed;
  uint32_t edges[4];
  portENTER_CRITICAL(&mux);
  a = activeAxis; f = fault; e = enabled; speed = velocity; left = remaining; window = sensing;
  for (int i = 0; i < 2; ++i) { p[i] = position[i]; z[i] = zeroed[i]; }
  for (int i = 0; i < 4; ++i) edges[i] = diagEdges[i];
  portEXIT_CRITICAL(&mux);
  String s = String("{\"type\":\"status\",\"fault\":\"") + faultText[f] +
    "\",\"enabled\":" + (e ? "true" : "false") + ",\"ready\":" + (ready ? "true" : "false") +
    ",\"axis\":" + a + ",\"speed\":" + String(speed, 1) + ",\"remaining\":" + left +
    ",\"owner\":" + owner + ",\"position\":[" + p[0] + "," + p[1] +
    "],\"zeroed\":[" + (z[0] ? "true" : "false") + "," + (z[1] ? "true" : "false") +
    "],\"window\":" + (window ? "true" : "false") + ",\"drivers\":[";
  for (int i = 0; i < 4; ++i) {
    if (i) s += ",";
    s += String("{\"name\":\"") + NAMES[i] + "\",\"address\":" + ADDRESS[i] +
      ",\"selected\":" + (PRESENT[i] ? "true" : "false") +
      ",\"seen\":" + (seen[i] ? "true" : "false") +
      ",\"configured\":" + (configured[i] ? "true" : "false") +
      ",\"age\":" + (millis() - sampleAt[i]) + ",\"drv\":" + drv[i] +
      ",\"gstat\":" + gstat[i] + ",\"sg\":" + sg[i] +
      ",\"diag\":" + digitalRead(DIAG_PINS[i]) + ",\"edges\":" + edges[i] + "}";
  }
  s += "]}"; ws.broadcastTXT(s);
}
void exportSettings() {
  Serial.println("Applied settings (RAM only):"); Serial.println(settingsJSON());
  for (int i = 0; i < 4; ++i)
    Serial.printf("%s address=%d selected=%d seen=%d configured=%d\n", NAMES[i], ADDRESS[i], PRESENT[i], seen[i], configured[i]);
}
void onSocket(uint8_t client, WStype_t type, uint8_t *payload, size_t length) {
  if (type == WStype_DISCONNECTED && owner == client) { latchFault(2); owner = -1; return; }
  if (type == WStype_CONNECTED) {
    String s = String("{\"type\":\"hello\",\"id\":") + client + ",\"settings\":" + settingsJSON() + "}";
    ws.sendTXT(client, s); return;
  }
  if (type != WStype_TEXT || length > 64) return;
  String m; for (size_t i = 0; i < length; ++i) m += char(payload[i]);
  if (m == "disable") { stopMotion(true); owner = -1; return; }
  if (m == "stop") { stopMotion(false); return; }
  if (m == "export") { exportSettings(); notice(client, "Applied settings printed to Serial."); return; }
  if (m == "beat" && owner == client) {
    portENTER_CRITICAL(&mux); heartbeat = esp_timer_get_time(); portEXIT_CRITICAL(&mux); return;
  }
  if (owner >= 0 && owner != client) { notice(client, "Another browser owns the controls."); return; }
  if (m == "arm") {
    portENTER_CRITICAL(&mux); const bool canArm = !enabled && !fault; portEXIT_CRITICAL(&mux);
    if (!canArm || !ready) { notice(client, "Disable and Apply / clear fault; every selected driver must pass."); return; }
    if (!setCurrentMode(-1)) return;
    owner = client;
    portENTER_CRITICAL(&mux);
    if (!fault) { heartbeat = esp_timer_get_time(); enabled = true; digitalWrite(EN, LOW); }
    portEXIT_CRITICAL(&mux); return;
  }
  // Wire protocol: zero:v | jog:v:+ | step:h:-100 | goto:v:250
  const int colon = m.indexOf(':');
  if (colon < 0) return;
  const String op = m.substring(0, colon);
  const String tail = m.substring(colon + 1);
  if (!tail.length() || (tail[0] != 'v' && tail[0] != 'h')) return;
  const int axis = tail[0] == 'v' ? 0 : 1;
  if (op == "zero" && tail.length() == 1) {
    portENTER_CRITICAL(&mux);
    const bool ok = enabled && !fault && activeAxis < 0 && owner == client &&
      (PRESENT[axis * 2] || PRESENT[axis * 2 + 1]);
    if (ok) { position[axis] = 0; zeroed[axis] = true; }
    portEXIT_CRITICAL(&mux);
    notice(client, ok ? "Stage zero set." : "Arm and stop both stages before setting zero."); return;
  }
  if (tail.length() < 3 || tail[1] != ':') return;
  const String raw = tail.substring(2);
  if (op == "jog" && (raw == "+" || raw == "-")) {
    startMove(client, axis, raw == "+" ? cfg.maxTravel : -cfg.maxTravel, false); return;
  }
  long value;
  if ((op == "step" || op == "goto") && parseNumber(raw, -POSITION_BOUND, POSITION_BOUND, value))
    startMove(client, axis, value, op == "goto");
  else notice(client, "Invalid command or integer step value.");
}
void applySettings() {
  portENTER_CRITICAL(&mux); const bool busy = enabled || activeAxis >= 0; portEXIT_CRITICAL(&mux);
  if (busy) { http.send(409, "text/plain", "Disable outputs before applying."); return; }
  Settings next = cfg;
  struct Field { const char *name; int *value; int low; int high; } fields[] = {
    {"current",&next.current,300,1000}, {"hold",&next.hold,30,100},
    {"precharge",&next.precharge,100,2000}, {"vSpeed",&next.vSpeed,20,600},
    {"vAccel",&next.vAccel,10,1000}, {"hSpeed",&next.hSpeed,20,600},
    {"hAccel",&next.hAccel,10,1000}, {"start",&next.start,5,100},
    {"maxTravel",&next.maxTravel,1,100000}, {"sgthrs",&next.sgthrs,0,255},
    {"senseMin",&next.senseMin,50,600}, {"settle",&next.settle,100,2000}
  };
  for (auto &field : fields) {
    long value;
    if (!parseNumber(http.arg(field.name), field.low, field.high, value)) {
      http.send(400, "text/plain", String("Invalid ") + field.name); return;
    }
    *field.value = value;
  }
  if (next.start > next.vSpeed || next.start > next.hSpeed ||
      (http.arg("stopDiag") != "0" && http.arg("stopDiag") != "1")) {
    http.send(400, "text/plain", "Start speed must not exceed either max speed; DIAG must be boolean."); return;
  }
  next.stopDiag = http.arg("stopDiag") == "1";
  portENTER_CRITICAL(&mux); cfg = next; portEXIT_CRITICAL(&mux);
  ready = false;
  const bool ok = configureAll() && pulseTimer;
  portENTER_CRITICAL(&mux); fault = ok ? 0 : (pulseTimer ? 3 : 5); portEXIT_CRITICAL(&mux);
  ready = ok; owner = -1;
  http.send(ok ? 200 : 503, "text/plain", ok ? "Applied to all selected drivers. Arm explicitly." : "Settings saved in RAM, but driver checks failed; outputs disabled.");
}
void setup() {
  digitalWrite(EN, HIGH); pinMode(EN, OUTPUT);
  for (int p : {25, 14, 32, 26, 33}) { digitalWrite(p, LOW); pinMode(p, OUTPUT); }
  // GPIO13 stays reserved; no limit pins are configured or read.
  Serial.begin(115200); uart.begin(115200, SERIAL_8N1, 16, 17);
  for (int i = 0; i < 4; ++i) {
    pinMode(DIAG_PINS[i], INPUT);
    if (PRESENT[i]) {
      drivers[i]->begin();
      attachInterruptArg(DIAG_PINS[i], diagISR, reinterpret_cast<void *>(intptr_t(i)), RISING);
    }
  }
  ready = configureAll();
  if (!ready) fault = 3;
  esp_timer_create_args_t args = {};
  args.callback = motionTick; args.dispatch_method = ESP_TIMER_TASK; args.name = "stages";
  args.skip_unhandled_events = true;
  if (esp_timer_create(&args, &pulseTimer) != ESP_OK || esp_timer_start_periodic(pulseTimer, 100) != ESP_OK) {
    pulseTimer = nullptr; ready = false; fault = 5;
  }
  WiFi.mode(WIFI_STA); WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  http.on("/", []() { http.sendHeader("Cache-Control", "no-store"); http.send_P(200, "text/html", CONTROL_PAGE); });
  http.on("/settings", HTTP_POST, applySettings);
  http.begin(); ws.begin(); ws.onEvent(onSocket);
  Serial.println("Four-motor test: no limits/homing. Serial p prints settings.");
}
void loop() {
  http.handleClient(); ws.loop();
  static bool connected = false;
  const bool online = WiFi.status() == WL_CONNECTED;
  if (online && !connected) {
    Serial.printf("Open http://%s or http://cnc-press-brake.local\n", WiFi.localIP().toString().c_str());
    if (MDNS.begin("cnc-press-brake")) MDNS.addService("http", "tcp", 80);
  }
  if (!online && connected) { latchFault(2); owner = -1; }
  connected = online;
  portENTER_CRITICAL(&mux); const int a = activeAxis; const bool e = enabled; portEXIT_CRITICAL(&mux);
  if (a < 0 && runHoldAxis >= 0 && ready) setCurrentMode(-1);
  if (!e) owner = -1;
  if (Serial.available() && Serial.read() == 'p') exportSettings();
  static uint32_t lastPoll = 0, lastStatus = 0;
  static int cursor = 0;
  if (millis() - lastPoll >= 100) {
    lastPoll = millis();
    for (int n = 0; n < 4; ++n) {
      const int i = cursor; cursor = (cursor + 1) % 4;
      if (PRESENT[i]) { pollDriver(i); break; }
    }
  }
  if (millis() - lastStatus >= 150) { lastStatus = millis(); sendTelemetry(); }
  delay(1);
}
