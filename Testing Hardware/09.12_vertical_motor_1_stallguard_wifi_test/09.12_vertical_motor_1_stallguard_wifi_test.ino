/* Single-motor bench test; see README.md before powering the mechanics.
   ESP32 DevKit V1, BTT TMC2209 V1.3 address 0, TMCStepper + WebSockets.
   Full steps (TMCStepper encodes this as microsteps(0)), interpolation OFF,
   CoolStep OFF, StealthChop ON. No automatic current boost or OTP writes.
   Other STEP pins stay LOW. Global EN still enables every connected module!
*/
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <ESPmDNS.h>
#include <TMCStepper.h>
#include <esp_timer.h>
#include <math.h>
#include "control_page.h"

// Same network as 09.07; credentials are filled from that sketch locally.
const char *WIFI_SSID = "Rhymes with Donna";
const char *WIFI_PASSWORD = "!Bbrosgaming2020";
constexpr int STEP = 25, DIR = 26, EN = 27, DIAG = 39;
constexpr int LIMIT1 = 34, LIMIT2 = 35;
constexpr uint32_t HEARTBEAT_US = 750000;
struct Settings {
  int current = 600, hold = 50, precharge = 400;
  int speed = 250, accel = 100, start = 20;
  int sgthrs = 0, senseMin = 200, settle = 300;
  int sgZero = 510, sgFull = 0, maxTravel = 600;
  bool stopDiag = false, limits = true;
} cfg;
HardwareSerial uart(2);
TMC2209Stepper driver(&uart, 0.110f, 0);
WebServer http(80);
WebSocketsServer ws(81);
esp_timer_handle_t pulseTimer;
portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
// All timer/ISR shared state is accessed under mux. UART and network use loop only.
bool enabled = false, sensing = false;
int direction = 0, fault = 0;
float velocity = 0;
uint32_t diagEdges = 0;
int32_t position = 0, moveSteps = 0;
int64_t heartbeat = 0, prechargeUntil = 0, lastTick = 0, nextStep = 0, cruiseSince = 0;
int owner = -1;
bool ready = false, runHold = false;
uint32_t drv = 0, gstat = 0, tstep = 0, lastPoll = 0;
uint16_t sg = 0;
bool sampleValid = false;
String loadReason = "motor stopped";
const char *faultText[] = {"none", "DIAG stall", "NC limit open", "browser watchdog/disconnect",
  "UART/configuration lost", "driver temperature/short/undervoltage", "travel budget reached", "timer unavailable"};

void IRAM_ATTR diagISR() {
  portENTER_CRITICAL_ISR(&mux);
  ++diagEdges;
  if (enabled && direction && sensing && cfg.stopDiag && !fault) fault = 1;
  portEXIT_CRITICAL_ISR(&mux);
}

// High-priority esp_timer task: no UART, heap, browser work, or catch-up bursts.
// 100 us service resolution; a late callback delays motion, never speeds it up.
void motionTick(void *) {
  const int64_t now = esp_timer_get_time();
  portENTER_CRITICAL(&mux);
  if (enabled) {
    if (now - heartbeat > HEARTBEAT_US) {
      if (!fault) fault = 3;
      enabled = false; digitalWrite(EN, HIGH);
    }
    if (cfg.limits && (digitalRead(LIMIT1) || digitalRead(LIMIT2)) && !fault) fault = 2;
  }
  if (fault) {
    direction = 0; velocity = 0; sensing = false;
    digitalWrite(STEP, LOW);
    // Retain holding torque after DIAG, travel-budget, or limit stops.
    // Communication/electrical faults disable outputs; support vertical loads.
    if (fault == 3 || fault == 4 || fault == 5 || fault == 7) {
      enabled = false; digitalWrite(EN, HIGH);
    }
  }
  if (!enabled || !direction || now < prechargeUntil) { lastTick = now; portEXIT_CRITICAL(&mux); return; }
  const float dt = fminf((now - lastTick) / 1000000.0f, 0.002f);
  lastTick = now;
  velocity = fminf(cfg.speed, velocity + cfg.accel * dt);
  if (velocity >= cfg.speed) { if (!cruiseSince) cruiseSince = now; }
  else cruiseSince = 0;
  sensing = cruiseSince && now - cruiseSince >= cfg.settle * 1000LL && velocity >= cfg.senseMin;
  if (sensing && cfg.stopDiag && digitalRead(DIAG)) {
    if (!fault) fault = 1;
    direction = 0; velocity = 0; sensing = false;
    portEXIT_CRITICAL(&mux); return;
  }
  if (now >= nextStep) {
    digitalWrite(STEP, HIGH);
    delayMicroseconds(3);
    digitalWrite(STEP, LOW);
    position += direction;
    ++moveSteps;
    nextStep = now + static_cast<int64_t>(1000000.0f / velocity);
    if (moveSteps >= cfg.maxTravel) {
      fault = 6; direction = 0; velocity = 0; sensing = false;
    }
  }
  portEXIT_CRITICAL(&mux);
}

void stopMotion(bool disable) {
  portENTER_CRITICAL(&mux);
  direction = 0; velocity = 0; sensing = false; cruiseSince = 0;
  digitalWrite(STEP, LOW);
  if (disable) { enabled = false; digitalWrite(EN, HIGH); }
  portEXIT_CRITICAL(&mux);
  sampleValid = false;
}

void latchFault(int code) {
  portENTER_CRITICAL(&mux);
  if (!fault) fault = code;
  portEXIT_CRITICAL(&mux);
  stopMotion(code == 3 || code == 4 || code == 5 || code == 7);
}

bool configurationMatches() {
  const uint32_t gc = driver.GCONF();
  const uint32_t cc = driver.CHOPCONF();
  const uint32_t pc = driver.PWMCONF();
  return (gc & 0xC7) == 0xC0 && ((cc >> 24) & 15) == 8 &&
    !(cc & (1UL << 28)) && (cc & 15) == 4 &&
    (pc & (3UL << 18)) == (3UL << 18);
}

bool configureDriver() {
  if (((driver.IOIN() >> 24) & 255) != 0x21 || driver.test_connection() != 0) return false;
  const uint8_t before = driver.IFCNT();
  driver.pdn_disable(true); driver.mstep_reg_select(true);
  driver.I_scale_analog(false); driver.internal_Rsense(false);
  driver.rms_current(cfg.current, cfg.hold / 100.0f);
  driver.iholddelay(8); driver.TPOWERDOWN(20);
  driver.toff(4); driver.blank_time(24);
  driver.microsteps(0); driver.intpol(false); driver.dedge(false);
  driver.en_spreadCycle(false); driver.TPWMTHRS(0);
  driver.pwm_autoscale(true); driver.pwm_autograd(true);
  driver.semin(0); driver.SGTHRS(cfg.sgthrs);
  // TSTEP is normalized to 256 microsteps, even when external STEP is fullstep.
  driver.TCOOLTHRS(static_cast<uint32_t>(12000000.0f / (256.0f * cfg.senseMin)));
  driver.VACTUAL(0);
  const uint8_t writes = static_cast<uint8_t>(driver.IFCNT() - before);
  const uint32_t gc = driver.GCONF(), cc = driver.CHOPCONF();
  // Require acknowledged writes and actual register readback. Do not depend on
  // an exact write count, which can change between TMCStepper library versions.
  const bool ok = writes > 0 && configurationMatches();
  Serial.printf("CONFIG %s: IFCNT accepted %u writes; GCONF=%08lX CHOPCONF=%08lX\n", ok ? "PASS" : "FAIL", writes, gc, cc);
  runHold = false;
  return ok;
}

bool setCurrentMode(bool fullHold) {
  const uint8_t before = driver.IFCNT();
  // Force IHOLD=IRUN before precharge: simply enabling would only give IHOLD.
  driver.rms_current(cfg.current, fullHold ? 1.0f : cfg.hold / 100.0f);
  if (static_cast<uint8_t>(driver.IFCNT() - before) == 0) { latchFault(4); ready = false; return false; }
  runHold = fullHold;
  return true;
}

void pollDriver() {
  const uint32_t io = driver.IOIN();
  drv = driver.DRV_STATUS(); gstat = driver.GSTAT();
  sg = driver.SG_RESULT(); tstep = driver.TSTEP();
  if (((io >> 24) & 255) != 0x21 || drv == 0xFFFFFFFF || sg > 510) { ready = false; latchFault(4); }
  // DRV_STATUS bits 0..5: otpw, ot, s2ga/b, s2vsa/b. Open-load bits 6/7 are telemetry only.
  if ((drv & 0x3F) || (gstat & 6)) latchFault(5);
  if ((gstat & 1) && ready) {
    // RESET is a sticky history flag and is normally set after power-up. Only
    // call it configuration loss when the live configuration no longer matches.
    if (configurationMatches()) {
      driver.GSTAT(1);
      gstat = driver.GSTAT();
    } else {
      Serial.println("FAULT: GSTAT reset flag set and live configuration no longer matches.");
      ready = false; latchFault(4);
    }
  }
  portENTER_CRITICAL(&mux);
  sampleValid = ready && enabled && direction && sensing && !fault &&
    (drv & (1UL << 30)) && !(drv & (1UL << 31)) && tstep > 0 &&
    tstep <= static_cast<uint32_t>(12000000.0f / (256.0f * cfg.senseMin));
  portEXIT_CRITICAL(&mux);
  if (fault) loadReason = String("fault: ") + faultText[fault];
  else if (!ready) loadReason = "driver configuration unavailable";
  else if (!enabled) loadReason = "outputs disabled";
  else if (!direction) loadReason = "motor stopped";
  else if (!sensing) loadReason = velocity < cfg.senseMin ? "below sensing speed" : "accelerating / settling";
  else if (!(drv & (1UL << 30))) loadReason = "driver is not in StealthChop";
  else if (drv & (1UL << 31)) loadReason = "driver reports standstill";
  else if (!tstep) loadReason = "no STEP timing measurement";
  else loadReason = sampleValid ? "live" : "outside validated sensing window";
}

String settingsJSON() {
  return String("{\"current\":") + cfg.current + ",\"hold\":" + cfg.hold + ",\"precharge\":" + cfg.precharge +
    ",\"speed\":" + cfg.speed + ",\"accel\":" + cfg.accel + ",\"start\":" + cfg.start +
    ",\"sgthrs\":" + cfg.sgthrs + ",\"senseMin\":" + cfg.senseMin + ",\"settle\":" + cfg.settle +
    ",\"sgZero\":" + cfg.sgZero + ",\"sgFull\":" + cfg.sgFull + ",\"maxTravel\":" + cfg.maxTravel +
    ",\"stopDiag\":" + (cfg.stopDiag ? "true" : "false") + ",\"limits\":" + (cfg.limits ? "true" : "false") + "}";
}

void exportSettings() {
  Serial.println("\n=== MOTOR 1 SETTINGS (volatile; copy to save) ===");
  Serial.println(settingsJSON());
  Serial.println("Fullstep; intpol=false; CoolStep=OFF; StealthChop; address=0; Rsense=0.110 ohm");
  Serial.printf("Actual quantized run ~%u mA RMS; requested hold %d%%; TCOOLTHRS=%lu\n",
    driver.cs2rms(driver.irun()), cfg.hold, driver.TCOOLTHRS());
  Serial.println("Supply current is NOT measured or capped by this sketch.\n=== END SETTINGS ===");
}

void sendTelemetry() {
  portENTER_CRITICAL(&mux);
  const int f = fault, d = direction;
  const bool e = enabled, window = sensing;
  const float v = velocity;
  const uint32_t edges = diagEdges;
  const int32_t pos = position;
  portEXIT_CRITICAL(&mux);
  const float load = constrain(100.0f * (cfg.sgZero - int(sg)) / (cfg.sgZero - cfg.sgFull), 0.0f, 100.0f);
  String s = String("{\"type\":\"status\",\"fault\":\"") + faultText[f] + "\",\"enabled\":" + (e ? "true" : "false") +
    ",\"direction\":" + d + ",\"speed\":" + String(v, 1) + ",\"position\":" + pos +
    ",\"sg\":" + sg + ",\"load\":" + (sampleValid ? String(load, 1) : "null") +
    ",\"loadReason\":\"" + loadReason + "\"" +
    ",\"diag\":" + digitalRead(DIAG) + ",\"edges\":" + edges + ",\"window\":" + (window ? "true" : "false") +
    ",\"limit1\":" + digitalRead(LIMIT1) + ",\"limit2\":" + digitalRead(LIMIT2) +
    ",\"drv\":\"0x" + String(drv, HEX) + "\",\"gstat\":" + gstat + ",\"tstep\":" + tstep +
    ",\"otpw\":" + (drv & 1) + ",\"ot\":" + ((drv >> 1) & 1) +
    ",\"shorts\":" + ((drv >> 2) & 15) + ",\"openLoad\":" + ((drv >> 6) & 3) +
    ",\"cs\":" + ((drv >> 16) & 31) + ",\"owner\":" + owner + "}";
  ws.broadcastTXT(s);
  static int reported = -1;
  if (reported != f) { reported = f; Serial.printf("STATE fault=%s\n", faultText[f]); }
}

void onSocket(uint8_t client, WStype_t type, uint8_t *payload, size_t length) {
  if (type == WStype_DISCONNECTED && owner == client) { latchFault(3); owner = -1; return; }
  if (type == WStype_CONNECTED) {
    String hello = String("{\"type\":\"hello\",\"id\":") + client + ",\"settings\":" + settingsJSON() + "}";
    ws.sendTXT(client, hello); return;
  }
  if (type != WStype_TEXT || length > 30) return;
  String m; for (size_t i = 0; i < length; ++i) m += char(payload[i]);
  if (m == "disable") { stopMotion(true); owner = -1; return; }
  if (m == "stop") { stopMotion(false); return; }
  if (m == "export") { exportSettings(); ws.sendTXT(client, "{\"notice\":\"Settings printed to Serial at 115200 baud.\"}"); return; }
  if (m == "beat" && owner == client) {
    portENTER_CRITICAL(&mux); heartbeat = esp_timer_get_time(); portEXIT_CRITICAL(&mux); return;
  }
  if (owner >= 0 && owner != client) return;
  portENTER_CRITICAL(&mux);
  const bool wasEnabled = enabled;
  const int wasFault = fault, wasDirection = direction;
  portEXIT_CRITICAL(&mux);
  if (m == "arm") {
    if (!ready || wasFault || wasEnabled) return;
    if (cfg.limits && (digitalRead(LIMIT1) || digitalRead(LIMIT2))) { latchFault(2); return; }
    if (!setCurrentMode(false)) return;
    owner = client;
    portENTER_CRITICAL(&mux); heartbeat = esp_timer_get_time(); enabled = true; digitalWrite(EN, LOW); portEXIT_CRITICAL(&mux);
  } else if (m == "up" || m == "down") {
    if (!wasEnabled || wasFault || wasDirection || owner != client) return;
    if (!setCurrentMode(true)) return;
    portENTER_CRITICAL(&mux);
    // A watchdog may have expired during UART; never resurrect a faulted command.
    if (!fault && enabled) {
      direction = m == "up" ? 1 : -1;
      digitalWrite(DIR, direction > 0 ? HIGH : LOW);
      velocity = cfg.start; moveSteps = 0; sensing = false; cruiseSince = 0;
      prechargeUntil = esp_timer_get_time() + cfg.precharge * 1000LL;
      nextStep = prechargeUntil; lastTick = esp_timer_get_time();
    }
    portEXIT_CRITICAL(&mux);
  }
}

void applySettings() {
  portENTER_CRITICAL(&mux); const bool busy = enabled || direction; portEXIT_CRITICAL(&mux);
  if (busy) { http.send(409, "text/plain", "Disable outputs before applying or clearing faults."); return; }
  Settings next = cfg;
  struct Field { const char *name; int *value; int low; int high; } fields[] = {
    {"current", &next.current, 300, 1000}, {"hold", &next.hold, 30, 100},
    {"precharge", &next.precharge, 100, 2000}, {"speed", &next.speed, 20, 600},
    {"accel", &next.accel, 10, 1000}, {"start", &next.start, 5, 100},
    {"sgthrs", &next.sgthrs, 0, 255}, {"senseMin", &next.senseMin, 50, 600},
    {"settle", &next.settle, 100, 2000}, {"sgZero", &next.sgZero, 1, 510},
    {"sgFull", &next.sgFull, 0, 509}, {"maxTravel", &next.maxTravel, 20, 2000}
  };
  for (auto &field : fields) {
    String raw = http.arg(field.name); char *end;
    long v = strtol(raw.c_str(), &end, 10);
    if (!raw.length() || *end || v < field.low || v > field.high) {
      http.send(400, "text/plain", String("Invalid ") + field.name); return;
    }
    *field.value = v;
  }
  if (next.start > next.speed || next.sgZero <= next.sgFull ||
      (http.arg("stopDiag") != "0" && http.arg("stopDiag") != "1") ||
      (http.arg("limits") != "0" && http.arg("limits") != "1")) {
    http.send(400, "text/plain", "Require start <= speed, SG at 0% > SG at 100%, and boolean switches."); return;
  }
  next.stopDiag = http.arg("stopDiag") == "1"; next.limits = http.arg("limits") == "1";
  if (next.limits && (digitalRead(LIMIT1) || digitalRead(LIMIT2))) {
    http.send(409, "text/plain", "NC limits are open. Close switches or explicitly select detached bench mode."); return;
  }
  portENTER_CRITICAL(&mux); cfg = next; portEXIT_CRITICAL(&mux);
  ready = false;
  driver.GSTAT(7);  // Clear old power-up/fault history before applying settings.
  bool ok = configureDriver();
  const uint32_t status = driver.DRV_STATUS();
  const uint8_t afterGstat = driver.GSTAT();
  ok = ok && !(status & 0x3F) && !(afterGstat & 6) && pulseTimer;
  Serial.printf("APPLY %s: GSTAT=%u DRV_STATUS=%08lX\n", ok ? "PASS" : "FAIL", afterGstat, status);
  portENTER_CRITICAL(&mux); fault = ok ? 0 : 4; portEXIT_CRITICAL(&mux);
  ready = ok; owner = -1;
  http.send(ok ? 200 : 503, "text/plain", ok ? "Applied; fault cleared. Arm explicitly to enable." : "Driver check failed; outputs remain disabled.");
}

void setup() {
  pinMode(EN, OUTPUT); digitalWrite(EN, HIGH);
  for (int p : {25, 14, 32, 13, 26, 33}) { pinMode(p, OUTPUT); digitalWrite(p, LOW); }
  pinMode(DIAG, INPUT); pinMode(LIMIT1, INPUT); pinMode(LIMIT2, INPUT);
  Serial.begin(115200);
  uart.begin(115200, SERIAL_8N1, 16, 17);
  driver.begin();
  const uint8_t bootGstat = driver.GSTAT();
  Serial.printf("Boot GSTAT history=%u (reset=1 is normal immediately after power-up)\n", bootGstat);
  driver.GSTAT(7);
  ready = configureDriver();
  if (!ready) fault = 4;
  attachInterrupt(digitalPinToInterrupt(DIAG), diagISR, RISING);
  esp_timer_create_args_t args = {};
  args.callback = motionTick; args.dispatch_method = ESP_TIMER_TASK; args.name = "motor1";
  args.skip_unhandled_events = true;
  if (esp_timer_create(&args, &pulseTimer) != ESP_OK || esp_timer_start_periodic(pulseTimer, 100) != ESP_OK) {
    pulseTimer = nullptr; ready = false; fault = 7;
  }
  WiFi.mode(WIFI_STA); WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  http.on("/", []() { http.sendHeader("Cache-Control", "no-store"); http.send_P(200, "text/html", CONTROL_PAGE); });
  http.on("/settings", HTTP_POST, applySettings);
  http.begin(); ws.begin(); ws.onEvent(onSocket);
  Serial.println("Motor 1 fullstep StallGuard bench test. Waiting for Wi-Fi; Serial 115200. Send p to print settings.");
}

void loop() {
  http.handleClient(); ws.loop();
  static bool connected = false;
  const bool online = WiFi.status() == WL_CONNECTED;
  if (online && !connected) {
    Serial.printf("Open http://%s or http://cnc-press-brake.local\n", WiFi.localIP().toString().c_str());
    if (MDNS.begin("cnc-press-brake")) MDNS.addService("http", "tcp", 80);
  }
  if (!online && connected) { latchFault(3); owner = -1; }
  connected = online;
  portENTER_CRITICAL(&mux); const bool moving = direction != 0, e = enabled; portEXIT_CRITICAL(&mux);
  if (!moving && runHold && ready) setCurrentMode(false);
  if (!e) owner = -1;
  if (Serial.available() && Serial.read() == 'p') exportSettings();
  if (millis() - lastPoll >= 150) { lastPoll = millis(); pollDriver(); sendTelemetry(); }
  delay(1);
}
