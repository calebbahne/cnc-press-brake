/* Four-motor commissioning test. See README.md and electrical-quick-reference.md.
   Protocol 4 checkpoint. Positions count STEP pulses from a manual/switch home.
   Global EN energizes all physically connected drivers; support vertical loads.
*/
#include <Arduino.h>
#include <TMCStepper.h>
#include <esp_timer.h>
#include <math.h>
#include <errno.h>
// The PC serves the UI and relays its commands over the programming USB cable.

// EDIT THESE FOUR SWITCHES BEFORE UPLOADING. False means physically unplugged.
// Never unplug drivers/motors under power. A false switch is NOT electrical isolation.
constexpr bool VERTICAL_1_CONNECTED = true;   // UART 0, STEP 25
constexpr bool VERTICAL_2_CONNECTED = true;   // UART 2, STEP 14
constexpr bool HORIZONTAL_1_CONNECTED = true; // UART 1, shared STEP 32
constexpr bool HORIZONTAL_2_CONNECTED = true; // UART 3, shared STEP 32
// Optional individual shaft inversion for mirrored motor installations.
constexpr bool INVERT_V1 = true, INVERT_V2 = true; // motor direction
constexpr bool INVERT_H1 = false, INVERT_H2 = false;

// Time allowed without a browser heartbeat while outputs are armed.
// Increase this if brief Wi-Fi/browser delays cause watchdog faults.
constexpr uint32_t BROWSER_WATCHDOG_MS = 8000;

constexpr int LIMIT_PINS[3] = {34,35,36}; // NC to GND, external pull-ups; HIGH = tripped/open.
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
  int microsteps = 4;
  int current = 600, hold = 50, precharge = 400;
  int vSpeed = 500, vAccel = 200, hSpeed = 500, hAccel = 200, start = 20;
  int maxTravel = 10000, sgthrs = 0, senseMin = 100, settle = 300;
  int toolMax = 0;
  int homeSpeed = 200, homeBackoff = 200;
  bool vLimitHoming = true, hLimitHoming = false;
  bool stopDiag = false;
} cfg;
int stepsPerMm() { return 25 * cfg.microsteps; } // 200 full steps/rev / 8 mm lead.
bool switchHomingEnabled(int axis) { return axis == 0 ? cfg.vLimitHoming : cfg.hLimitHoming; }
HardwareSerial uart(2);
TMC2209Stepper v1(&uart, 0.110f, 0), v2(&uart, 0.110f, 2);
TMC2209Stepper h1(&uart, 0.110f, 1), h2(&uart, 0.110f, 3);
TMC2209Stepper *drivers[4] = {&v1, &v2, &h1, &h2};
esp_timer_handle_t pulseTimer = nullptr;
portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
// Timer-shared state: always access under mux. UART/network only in loop().
bool enabled = false, sensing = false;
bool manualMoveActive = false;
uint32_t requestId = 0, activeMove = 0, completedMove = 0;
int64_t holdAt = 0, homeStarted = 0, homeContactAt = 0;
int homing = 0, homePulses = 0, skewPulses = 0;
bool homeHit[2] = {false,false};
int activeAxis = -1, direction = 0, fault = 0;
int32_t position[2] = {0, 0}, remaining = 0;
bool zeroed[2] = {false, false};
float velocity = 0, moveSpeedCap = 0;
int64_t heartbeat = 0, prechargeUntil = 0, lastTick = 0, nextStep = 0, cruiseSince = 0;
uint32_t diagEdges[4] = {};
bool ready = false, seen[4] = {}, configured[4] = {};
bool startupDriverRetry = false;
uint8_t startupDriverAttempts = 0;
uint32_t nextStartupDriverRetry = 0;
uint32_t drv[4] = {}, gstat[4] = {}, sampleAt[4] = {};
uint32_t configGconf[4] = {}, configChopconf[4] = {}, configPwmconf[4] = {};
uint8_t ifcntBefore[4] = {}, ifcntAfter[4] = {};
bool configMatch[4] = {};
uint16_t sg[4] = {};
int owner = -1, runHoldAxis = -1;
const char *faultText[] = {"none", "DIAG stall", "USB/browser watchdog/disconnect",
  "UART/configuration lost", "driver temperature/short/undervoltage", "timer unavailable", "limit input triggered", "homing failed", "motion hold expired", "Y homing skew exceeds 1.5 mm"};

void stepsLow() {
  digitalWrite(25, LOW); digitalWrite(14, LOW); digitalWrite(32, LOW);
}
void haltLocked(bool disable) {
  if (homing && activeAxis >= 0) zeroed[activeAxis] = false;
  homing = 0; activeMove = 0;
  activeAxis = -1; direction = 0; velocity = 0; remaining = 0; sensing = false; manualMoveActive = false;
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
// Y home is the upper switches: Y is zero there and negative downward.
// X home is retracted: X increases toward the die.
int pinDirection(int axis, int logicalDirection) {
  return axis == 0 ? (logicalDirection < 0 ? HIGH : LOW) : (logicalDirection > 0 ? HIGH : LOW);
}
// Called only with mux held. Homing seeks toward each axis's switch end.
void homeTickLocked(int64_t now) {
  const int a = activeAxis, first = a == 0 ? 0 : 2, count = a == 0 ? 2 : 1;
  if (now - homeStarted > 120000000LL) { fault = 7; haltLocked(false); return; }
  if (now < nextStep) return;
  if (homing == 1) {
    // Coarse seek: move the coupled stage only until the first switch opens.
    bool any = false;
    for (int i=0;i<count;++i) any = any || digitalRead(LIMIT_PINS[first+i]);
    if (any) {
      homing=2; direction=a==0?-1:1; remaining=cfg.homeBackoff;
      digitalWrite(DIR_PINS[a],pinDirection(a,direction)); nextStep=now+5000; return;
    }
    if (++homePulses > cfg.maxTravel) { fault=7; haltLocked(false); return; }
    if (a==0) { digitalWrite(25,HIGH); digitalWrite(14,HIGH); }
    else digitalWrite(32,HIGH);
    delayMicroseconds(3); stepsLow();
  } else if (homing == 2) {
    if (a==0) { digitalWrite(25,HIGH); digitalWrite(14,HIGH); }
    else digitalWrite(32,HIGH);
    delayMicroseconds(3); stepsLow(); position[a] += direction;
    if (--remaining==0) {
      for (int i=0;i<count;++i) if (digitalRead(LIMIT_PINS[first+i])) { fault=7; haltLocked(false); return; }
      homing=3; direction=a==0?1:-1; homeHit[0]=homeHit[1]=false;
      homePulses=skewPulses=0; homeContactAt=0;
      digitalWrite(DIR_PINS[a],pinDirection(a,direction)); nextStep=now+5000; return;
    }
  } else if (homing == 3) {
    for (int i=0;i<count;++i) homeHit[i]=digitalRead(LIMIT_PINS[first+i]);
    const bool all=homeHit[0] && (count==1 || homeHit[1]);
    if (all) {
      if (!homeContactAt) homeContactAt=now;
      if (now-homeContactAt<20000) { nextStep=now+1000; return; }
      position[a]=0; homeContactAt=0; direction=a==0?-1:1;
      remaining=a==0?2*stepsPerMm():cfg.homeBackoff; homing=4;
      digitalWrite(DIR_PINS[a],pinDirection(a,direction)); nextStep=now+5000; return;
    }
    homeContactAt=0;
    if (++homePulses>cfg.maxTravel) { fault=7; haltLocked(false); return; }
    if (a==0) {
      if (!homeHit[0]) digitalWrite(25,HIGH);
      if (!homeHit[1]) digitalWrite(14,HIGH);
      if ((homeHit[0] != homeHit[1]) && ++skewPulses > int(ceilf(1.5f*stepsPerMm()))) {
        fault=9; haltLocked(false); return;
      }
    } else digitalWrite(32,HIGH);
    delayMicroseconds(3); stepsLow();
  } else {
    if (a==0) { digitalWrite(25,HIGH); digitalWrite(14,HIGH); }
    else digitalWrite(32,HIGH);
    delayMicroseconds(3); stepsLow(); position[a]+=direction;
    if (--remaining==0) {
      zeroed[a]=true; homing=0; completedMove=activeMove; haltLocked(false); return;
    }
  }
  const int speed=homing==3?max(cfg.start,cfg.homeSpeed/4):cfg.homeSpeed;
  nextStep=now+1000000LL/speed;
}
// No UART/network/heap or catch-up bursts in this 100 us timer callback.
void motionTick(void *) {
  const int64_t now = esp_timer_get_time();
  portENTER_CRITICAL(&mux);
  if (enabled && now - heartbeat > int64_t(BROWSER_WATCHDOG_MS) * 1000LL) {
    if (!fault) fault = 2;
    haltLocked(true);
  }
  if (activeAxis >= 0 && activeMove && now-holdAt > 350000LL) {
    if (!fault) fault=8;
  }
  if (fault) {
    zeroed[0] = false; zeroed[1] = false;
    haltLocked(fault != 1);
  }
  if (!enabled || activeAxis < 0 || now < prechargeUntil) {
    lastTick = now; portEXIT_CRITICAL(&mux); return;
  }
  if (homing) { homeTickLocked(now); portEXIT_CRITICAL(&mux); return; }
  const int a = activeAxis;
  if (switchHomingEnabled(a) && (a == 0 ? direction > 0 : direction < 0) &&
      (a==0 ? digitalRead(34)||digitalRead(35) : digitalRead(36))) {
    if (zeroed[a]) { completedMove=activeMove; haltLocked(false); }
    else { fault=6; haltLocked(false); }
    portEXIT_CRITICAL(&mux); return;
  }
  const int32_t nextPos = position[a] + direction;
  if (zeroed[a] && !manualMoveActive && a==0 && cfg.toolMax>0 && nextPos < -cfg.toolMax) {
    haltLocked(false); portEXIT_CRITICAL(&mux); return;
  }
  const float maxSpeed = moveSpeedCap;
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
    if (--remaining == 0) { completedMove=activeMove; haltLocked(false); } // Reaching a target is normal completion.
  }
  portEXIT_CRITICAL(&mux);
}
bool configurationMatches(int i) {
  auto &d = *drivers[i];
  const uint32_t gc = d.GCONF(), cc = d.CHOPCONF(), pc = d.PWMCONF();
  configGconf[i] = gc; configChopconf[i] = cc; configPwmconf[i] = pc;
  int mres = 8; for (int value=1; value<cfg.microsteps; value*=2) --mres;
  configMatch[i] = (gc & 0xCF) == (0xC0U | (INVERT[i] ? 8U : 0U)) &&
    int((cc >> 24) & 15) == mres && (cc & (1UL << 28)) &&
    (cc & 15) == 4 && (pc & (3UL << 18)) == (3UL << 18);
  return configMatch[i];
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
  d.microsteps(cfg.microsteps); d.intpol(true); d.dedge(false);
  d.en_spreadCycle(false); d.TPWMTHRS(0);
  d.pwm_autoscale(true); d.pwm_autograd(true);
  d.semin(0); d.SGTHRS(cfg.sgthrs);
  d.TCOOLTHRS(static_cast<uint32_t>(12000000.0f * cfg.microsteps / (256.0f * cfg.senseMin)));
  d.VACTUAL(0);
  ifcntBefore[i] = before;
  ifcntAfter[i] = d.IFCNT();
  const bool acknowledged = static_cast<uint8_t>(ifcntAfter[i] - before) > 0;
  drv[i] = d.DRV_STATUS(); gstat[i] = d.GSTAT(); sampleAt[i] = millis();
  const bool registersMatch = configurationMatches(i);
  configured[i] = acknowledged && registersMatch &&
    drv[i] != 0xFFFFFFFF && !(drv[i] & 0x3F) && !(gstat[i] & 6);
  Serial.printf("%s UART %d: %s | IFCNT %u->%u | GCONF 0x%08lX CHOPCONF 0x%08lX PWMCONF 0x%08lX | register match %d | DRV 0x%08lX GSTAT %lu\n",
    NAMES[i], ADDRESS[i], configured[i] ? "CONFIG PASS" : "CONFIG FAIL",
    ifcntBefore[i], ifcntAfter[i],
    (unsigned long)configGconf[i], (unsigned long)configChopconf[i],
    (unsigned long)configPwmconf[i], configMatch[i],
    (unsigned long)drv[i], (unsigned long)gstat[i]);
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
void acknowledge(uint8_t client, bool ok, const char *message) {
  String s=String("{\"type\":\"result\",\"id\":")+requestId+",\"ok\":"+(ok?"true":"false")+",\"message\":\""+message+"\"}";
  Serial.print('@'); Serial.println(s);
}
void notice(uint8_t client, const char *message) {
  if (requestId) { acknowledge(client,false,message); return; }
  String s = String("{\"notice\":\"") + message + "\"}"; Serial.print('@'); Serial.println(s);
}
void startMove(uint8_t client, int axis, long amount, bool absolute, bool isHome, int speedLimit = 0, bool manualFree = false) {
  portENTER_CRITICAL(&mux);
  const bool allowed = enabled && !fault && activeAxis < 0 && owner == client;
  const int32_t pos = position[axis];
  const bool hasZero = zeroed[axis];
  portEXIT_CRITICAL(&mux);
  if (!allowed) { notice(client, "Arm first; stop the current stage before another move."); return; }
  if (!PRESENT[axis * 2] || !PRESENT[axis * 2 + 1]) { notice(client, "Both drivers must be selected for a coupled stage."); return; }
  if (!requestId) { notice(client,"Protocol 4 requires an identified held move."); return; }
  if (isHome) {
    if (!switchHomingEnabled(axis) || !PRESENT[axis*2] || !PRESENT[axis*2+1]) { notice(client,"Switch homing is not enabled for this axis or both stage drivers are unavailable."); return; }
  }
  if (absolute && !hasZero) { notice(client, "Set this stage zero first."); return; }
  const int64_t delta = absolute ? int64_t(amount) - pos : amount;
  const int64_t target = int64_t(pos) + delta;
  if (delta > cfg.maxTravel || delta < -cfg.maxTravel || target > POSITION_BOUND || target < -POSITION_BOUND) {
    notice(client, "Move exceeds the per-command step budget or position range."); return;
  }
  if (!isHome && !manualFree && !hasZero && (absolute || abs(amount)>stepsPerMm())) { notice(client,"Unhomed: use held setup increments of at most 1 mm, then set home."); return; }
  if (!isHome && !manualFree && hasZero && axis==0 && cfg.toolMax>0 && target < -cfg.toolMax) {
    notice(client,"Target exceeds the installed tooling depth limit."); return;
  }
  if (!delta && !isHome) {
    portENTER_CRITICAL(&mux); completedMove=requestId; portEXIT_CRITICAL(&mux);
    acknowledge(client,true,"Already at target."); return;
  }
  if (!setCurrentMode(axis)) return;
  portENTER_CRITICAL(&mux);
  if (enabled && !fault && activeAxis < 0) {
    activeMove=requestId; holdAt=esp_timer_get_time(); manualMoveActive=manualFree;
    activeAxis = axis; direction = delta > 0 ? 1 : -1;
    if (isHome) {
      homing=1; zeroed[axis]=false; homeHit[0]=homeHit[1]=false;
      homePulses=skewPulses=0; homeStarted=holdAt; homeContactAt=0;
      direction=axis==0 ? 1 : -1;
    }
    remaining = static_cast<int32_t>(delta > 0 ? delta : -delta);
    digitalWrite(DIR_PINS[axis], pinDirection(axis,direction));
    moveSpeedCap = speedLimit ? speedLimit : (axis == 0 ? cfg.vSpeed : cfg.hSpeed);
    velocity = isHome ? cfg.homeSpeed : cfg.start; sensing = false; cruiseSince = 0;
    const int64_t now = esp_timer_get_time();
    prechargeUntil = now + cfg.precharge * 1000LL;
    nextStep = prechargeUntil; lastTick = now;
  }
  const bool accepted = activeMove==requestId;
  portEXIT_CRITICAL(&mux);
  acknowledge(client,accepted,accepted?"Move accepted.":"Move interrupted before acceptance.");
}
String settingsJSON() {
  String s = "{";
#define FIELD(name) s += "\"" #name "\":" + String(cfg.name) + ",";
  FIELD(microsteps) FIELD(current) FIELD(hold) FIELD(precharge)
  FIELD(vSpeed) FIELD(vAccel) FIELD(hSpeed) FIELD(hAccel) FIELD(start)
  FIELD(maxTravel) FIELD(sgthrs) FIELD(senseMin) FIELD(settle)
  FIELD(toolMax) FIELD(homeSpeed) FIELD(homeBackoff)
#undef FIELD
  s += "\"vLimitHoming\":" + String(cfg.vLimitHoming ? "true" : "false") + ",";
  s += "\"hLimitHoming\":" + String(cfg.hLimitHoming ? "true" : "false") + ",";
  return s + "\"stopDiag\":" + (cfg.stopDiag ? "true" : "false") + "}";
}
void sendTelemetry() {
  int a, f; bool e, z[2], window; int32_t p[2], left; float speed;
  uint32_t edges[4], move, done; int home;
  portENTER_CRITICAL(&mux);
  move=activeMove; done=completedMove; home=homing;
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
      ",\"configGconf\":" + configGconf[i] +
      ",\"configChopconf\":" + configChopconf[i] +
      ",\"configPwmconf\":" + configPwmconf[i] +
      ",\"configMatch\":" + (configMatch[i] ? "true" : "false") +
      ",\"ifcntBefore\":" + String(unsigned(ifcntBefore[i])) +
      ",\"ifcntAfter\":" + String(unsigned(ifcntAfter[i])) +
      ",\"diag\":" + digitalRead(DIAG_PINS[i]) + ",\"edges\":" + edges[i] + "}";
  }
  s += "],\"protocol\":4,\"stepsPerMm\":"+String(stepsPerMm())+",\"moveId\":"+move+",\"completedId\":"+done+",\"homing\":"+home;
  s += ",\"limits\":["+String(cfg.vLimitHoming?digitalRead(34):-1)+","+String(cfg.vLimitHoming?digitalRead(35):-1)+","+String(cfg.hLimitHoming?digitalRead(36):-1)+"]";
  s += ",\"transport\":\"usb\"";
  s += ",\"settings\":"+settingsJSON()+"}"; Serial.print('@'); Serial.println(s);
}
void exportSettings() {
  Serial.println("Applied settings (RAM only):"); Serial.println(settingsJSON());
  for (int i = 0; i < 4; ++i)
    Serial.printf("%s address=%d selected=%d seen=%d configured=%d\n", NAMES[i], ADDRESS[i], PRESENT[i], seen[i], configured[i]);
}
enum LinkEvent { LINK_DISCONNECTED, LINK_CONNECTED, LINK_TEXT };
void onLink(uint8_t client, int type, const String &input) {
  requestId=0;
  if (type == LINK_DISCONNECTED && owner == client) { latchFault(2); owner = -1; return; }
  if (type == LINK_CONNECTED) {
    // A new browser session must never inherit an armed session after a quick USB reconnect.
    if (owner == client) { latchFault(2); owner = -1; }
    String s = String("{\"type\":\"hello\",\"id\":") + client + ",\"protocol\":4,\"stepsPerMm\":"+String(stepsPerMm())+",\"settings\":" + settingsJSON() + "}";
    Serial.print('@'); Serial.println(s); return;
  }
  if (type != LINK_TEXT || input.length() > 96) return;
  String m = input;
  if (m.startsWith("hold:")) {
    long id;
    if (owner==client && parseNumber(m.substring(5),1,2147483647,id)) {
      portENTER_CRITICAL(&mux); if (activeMove==uint32_t(id)) holdAt=esp_timer_get_time(); portEXIT_CRITICAL(&mux);
    }
    return;
  }
  const int bar=m.indexOf('|');
  if (bar>=0) {
    long id; if (!parseNumber(m.substring(0,bar),1,2147483647,id)) return;
    requestId=uint32_t(id); m=m.substring(bar+1);
  }
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
  // Wire protocol: zero:v | jog:v:+:100 | jogstep:v:-:20 | goto:v:-250
  const int colon = m.indexOf(':');
  if (colon < 0) return;
  const String op = m.substring(0, colon);
  const String tail = m.substring(colon + 1);
  if (!tail.length() || (tail[0] != 'v' && tail[0] != 'h')) return;
  const int axis = tail[0] == 'v' ? 0 : 1;
  if (op == "home" && tail.length()==1) { startMove(client,axis,-1,false,true); return; }
  if (op == "zero" && tail.length() == 1) {
    if (switchHomingEnabled(axis)) { notice(client,"Switch homing is enabled for this axis; use switch Home."); return; }
    portENTER_CRITICAL(&mux);
    const bool ok = enabled && !fault && activeAxis < 0 && owner == client &&
      (PRESENT[axis * 2] || PRESENT[axis * 2 + 1]);
    if (ok) { position[axis] = 0; zeroed[axis] = true; }
    portEXIT_CRITICAL(&mux);
    notice(client, ok ? "Stage zero set." : "Arm and stop both stages before setting zero."); return;
  }
  if (tail.length() < 3 || tail[1] != ':') return;
  const String raw = tail.substring(2);
  if (op == "jog") {
    const int split = raw.indexOf(':');
    if (split != 1 || (raw[0] != '+' && raw[0] != '-')) { notice(client,"Jog requires direction and speed in pulses/s."); return; }
    long speed;
    const int stageMax = axis == 0 ? cfg.vSpeed : cfg.hSpeed;
    if (!parseNumber(raw.substring(2),cfg.start,stageMax,speed)) { notice(client,"Jog speed is outside the configured stage range."); return; }
    // Held manual jog has a per-command budget, but no assumed physical home/end stops.
    startMove(client,axis,(raw[0] == '+' ? 1 : -1) * cfg.maxTravel,false,false,int(speed),true);
    return;
  }
  if (op == "jogstep") {
    const int split = raw.indexOf(':');
    if (split != 1 || (raw[0] != '+' && raw[0] != '-')) { notice(client,"Distance jog requires direction and steps."); return; }
    long steps;
    if (!parseNumber(raw.substring(2),1,cfg.maxTravel,steps)) { notice(client,"Distance is outside the per-command step budget."); return; }
    startMove(client,axis,(raw[0] == '+' ? 1 : -1) * steps,false,false,0,true);
    return;
  }
  long value;
  if ((op == "step" || op == "goto" || op == "manual") && parseNumber(raw, -POSITION_BOUND, POSITION_BOUND, value))
    startMove(client, axis, value, op == "goto", false, 0, op == "manual");
  else notice(client, "Invalid command or integer step value.");
}
String formArg(const String &form, const char *key) {
  const String needle = String(key) + "=";
  int at = 0;
  while (at < form.length()) {
    const int end = form.indexOf('&', at);
    const int stop = end < 0 ? form.length() : end;
    if (form.substring(at, stop).startsWith(needle)) return form.substring(at + needle.length(), stop);
    at = stop + 1;
  }
  return "";
}
void settingsReply(long id, int code, const char *message) {
  Serial.print(F("@{\"type\":\"settingsResult\",\"id\":")); Serial.print(id);
  Serial.print(F(",\"code\":")); Serial.print(code);
  Serial.print(F(",\"message\":\"")); Serial.print(message); Serial.println(F("\"}"));
}
void applySettings(const String &form, long id) {
  portENTER_CRITICAL(&mux); const bool busy = enabled || activeAxis >= 0; portEXIT_CRITICAL(&mux);
  if (busy) { settingsReply(id,409,"Disable outputs before applying."); return; }
  Settings next = cfg;
  struct Field { const char *name; int *value; int low; int high; } fields[] = {
    {"microsteps",&next.microsteps,1,256},
    {"current",&next.current,300,1000}, {"hold",&next.hold,30,100},
    {"precharge",&next.precharge,100,2000}, {"vSpeed",&next.vSpeed,20,10000},
    {"vAccel",&next.vAccel,10,40000}, {"hSpeed",&next.hSpeed,20,10000},
    {"hAccel",&next.hAccel,10,40000}, {"start",&next.start,5,2000},
    {"maxTravel",&next.maxTravel,1,1000000}, {"sgthrs",&next.sgthrs,0,255},
    {"senseMin",&next.senseMin,1,10000}, {"settle",&next.settle,100,2000},
    {"toolMax",&next.toolMax,0,160000},
    {"homeSpeed",&next.homeSpeed,20,10000}, {"homeBackoff",&next.homeBackoff,20,12800}
  };
  for (auto &field : fields) {
    long value;
    if (!parseNumber(formArg(form,field.name), field.low, field.high, value)) {
      settingsReply(id,400,"Invalid motion setting."); return;
    }
    *field.value = value;
  }
  const bool validMicrosteps = next.microsteps==1 || next.microsteps==2 || next.microsteps==4 || next.microsteps==8 ||
    next.microsteps==16 || next.microsteps==32 || next.microsteps==64 || next.microsteps==128 || next.microsteps==256;
  if (!validMicrosteps || next.start > next.vSpeed || next.start > next.hSpeed ||
      (formArg(form,"stopDiag") != "0" && formArg(form,"stopDiag") != "1")) {
    settingsReply(id,400,"Microstepping, start speed, or DIAG value invalid."); return;
  }
  const String vHome = formArg(form,"vLimitHoming"), hHome = formArg(form,"hLimitHoming");
  if ((vHome!="0" && vHome!="1") || (hHome!="0" && hHome!="1") ||
      next.homeBackoff>next.maxTravel) {
    settingsReply(id,400,"Invalid homing mode, backoff, or tool envelope."); return;
  }
  next.vLimitHoming=vHome=="1"; next.hLimitHoming=hHome=="1";
  next.stopDiag = formArg(form,"stopDiag") == "1";
  portENTER_CRITICAL(&mux); cfg = next; portEXIT_CRITICAL(&mux);
  for (int i=0;i<4;++i) {
    detachInterrupt(DIAG_PINS[i]);
    if (PRESENT[i] && cfg.stopDiag)
      attachInterruptArg(DIAG_PINS[i],diagISR,reinterpret_cast<void *>(intptr_t(i)),RISING);
  }
  ready = false;
  const bool ok = configureAll() && pulseTimer;
  startupDriverRetry = false;
  portENTER_CRITICAL(&mux); fault = ok ? 0 : (pulseTimer ? 3 : 5); portEXIT_CRITICAL(&mux);
  ready = ok; owner = -1;
  settingsReply(id,ok ? 200 : 503, ok ? "Applied to all selected drivers. Arm explicitly." : "Driver checks failed; outputs disabled.");
}
void serviceUsb() {
  static String line;
  while (Serial.available()) {
    const char c = char(Serial.read());
    if (c == '\n') {
      if (line == "K") onLink(1,LINK_CONNECTED,"");
      else if (line == "X") onLink(1,LINK_DISCONNECTED,"");
      else if (line.startsWith("C:")) onLink(1,LINK_TEXT,line.substring(2));
      else if (line.startsWith("S:")) {
        const int split=line.indexOf(':',2); long id;
        if (split>2 && parseNumber(line.substring(2,split),1,2147483647,id)) applySettings(line.substring(split+1),id);
      }
      line="";
    } else if (c != '\r') {
      if (line.length()<4096) line+=c;
      else line="";
    }
  }
}
void setup() {
  digitalWrite(EN, HIGH); pinMode(EN, OUTPUT);
  for (int p : {25, 14, 32, 26, 33}) { digitalWrite(p, LOW); pinMode(p, OUTPUT); }
  for (int p : LIMIT_PINS) pinMode(p,INPUT); // External pull-ups required; ignored unless enabled.
  Serial.begin(115200); uart.begin(115200, SERIAL_8N1, 16, 17);
  for (int i = 0; i < 4; ++i) {
    pinMode(DIAG_PINS[i], INPUT);
    if (PRESENT[i]) {
      drivers[i]->begin();
      if (cfg.stopDiag) attachInterruptArg(DIAG_PINS[i], diagISR, reinterpret_cast<void *>(intptr_t(i)), RISING);
    }
  }
  ready = configureAll();
  if (!ready) {
    fault = 3;
    startupDriverRetry = true;
    startupDriverAttempts = 1;
    nextStartupDriverRetry = millis() + 1500;
    Serial.println("Driver configuration incomplete at boot; retrying while outputs remain disabled.");
  }
  esp_timer_create_args_t args = {};
  args.callback = motionTick; args.dispatch_method = ESP_TIMER_TASK; args.name = "stages";
  args.skip_unhandled_events = true;
  if (esp_timer_create(&args, &pulseTimer) != ESP_OK || esp_timer_start_periodic(pulseTimer, 100) != ESP_OK) {
    pulseTimer = nullptr; ready = false; fault = 5;
  }
  Serial.printf("Press Brake USB protocol 4: %dx microsteps, %d steps/mm. Axis-specific switch homing and DIAG off by default.\n", cfg.microsteps, stepsPerMm());
}
void loop() {
  serviceUsb();
  if (startupDriverRetry && int32_t(millis() - nextStartupDriverRetry) >= 0) {
    portENTER_CRITICAL(&mux); const bool safeToRetry = !enabled && activeAxis < 0;
    portEXIT_CRITICAL(&mux);
    if (safeToRetry) {
      ++startupDriverAttempts;
      Serial.printf("Startup driver configuration retry %u/5\n", startupDriverAttempts);
      const bool ok = configureAll();
      if (ok && pulseTimer) {
        portENTER_CRITICAL(&mux); if (fault == 3) fault = 0; portEXIT_CRITICAL(&mux);
        ready = true; startupDriverRetry = false;
        Serial.println("Driver configuration now ready; Arm is available.");
      } else if (startupDriverAttempts >= 5) {
        startupDriverRetry = false;
        Serial.println("Driver startup retries exhausted. Inspect diagnostic registers; Apply settings can retry while disabled.");
      } else nextStartupDriverRetry = millis() + 1500;
    }
  }
  portENTER_CRITICAL(&mux); const int a = activeAxis; const bool e = enabled; portEXIT_CRITICAL(&mux);
  if (a < 0 && runHoldAxis >= 0 && ready) setCurrentMode(-1);
  if (!e) owner = -1;
  static uint32_t lastPoll = 0, lastStatus = 0;
  static int cursor = 0;
  if (millis() - lastPoll >= 100) {
    lastPoll = millis();
    for (int n = 0; n < 4; ++n) {
      const int i = cursor; cursor = (cursor + 1) % 4;
      if (PRESENT[i]) { pollDriver(i); break; }
    }
  }
  if (millis() - lastStatus >= 300) { lastStatus = millis(); sendTelemetry(); }
  delay(1);
}
