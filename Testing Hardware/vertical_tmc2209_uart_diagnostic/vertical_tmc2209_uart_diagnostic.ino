/*
  vertical_tmc2209_uart_diagnostic.ino

  Interactive bring-up and tuning tool for the CNC press brake's two vertical
  BIGTREETECH TMC2209 V1.3 modules and ESP32 DevKit V1.

  REQUIRED LIBRARIES
    - TMCStepper by teemuatlut
    - AccelStepper by Mike McCauley

  REQUIRED WIRING (from electrical-quick-reference.md)
    GPIO25 -> vertical driver 1 STEP
    GPIO14 -> vertical driver 2 STEP
    GPIO26 -> both vertical DIR pins
    GPIO27 -> all drivers EN (active LOW), with 10k pull-up to 3V3
    GPIO17 -> 1k resistor -> shared TMC RX/PDN_UART bus
    GPIO16 -----------------> shared TMC RX/PDN_UART bus
    GPIO39 <- vertical driver 1 DIAG
    GPIO19 <- vertical driver 2 DIAG
    GPIO34 <- vertical limit 1 (NC switch, external pull-up, active HIGH)
    GPIO35 <- vertical limit 2 (NC switch, external pull-up, active HIGH)

  OPTIONAL VREF MONITORING FOR THIS VERTICAL-ONLY TEST
    vertical driver 1 auxiliary VREF -> GPIO32
    vertical driver 2 auxiliary VREF -> GPIO33

  GPIO32 and GPIO33 normally belong to the horizontal axis. Do not command the
  horizontal axis while this diagnostic is installed. VREF is normally about
  0.2-2.2 V and is safe for a 3.3 V ADC input. Never feed VM/24 V into an ESP32.

  TMC ADDRESS STRAPS
    Vertical 1: MS2=GND, MS1=GND      -> address 0
    Vertical 2: MS2=GND, MS1=3V3/VIO -> address 1

  IMPORTANT MEASUREMENT LIMITATION
    VREF is a static current-limit reference. It does NOT show instantaneous
    coil current and should not change between idle, motion, and load. The
    logged "CS mA" is the driver's present digital current scale converted to
    an estimated RMS current by TMCStepper. It is useful for seeing CoolStep
    act, but it is not a calibrated current sensor. For true coil current, use
    a DC-capable current probe or a differential scope across a sense resistor.

  SAFETY
    - Test at low speed/current with tooling clear and a hand on motor power.
    - Power OFF before changing motor, driver, UART, address, or VREF wiring.
    - The project's EN line is global: enabling here energizes all four drivers.
    - Limit inputs abort motion, but this is a diagnostic, not safety-rated logic.
    - Do not adjust the VREF potentiometer with a metal tool that can short it.

  FIRST TEST SEQUENCE
    1. Open Serial Monitor at 115200 and verify both UART connections say OK.
    2. Enter: status
    3. Enter: enable 1       (both vertical motors should become rigid)
    4. Enter: hold 100       (hold current = run current for diagnosis)
    5. Enter: precharge 1000 (energize one second before the first step)
    6. Enter: motor 1, then move 400; repeat with motor 2.
    7. Reduce speed/acceleration if a loaded motor cannot start:
         speed 150
         accel 300
    8. Compare unloaded and loaded SG values before enabling CoolStep.

  Type "help" for the complete command list.
*/

#include <Arduino.h>
#include <AccelStepper.h>
#include <TMCStepper.h>

// ----------------------------- Physical pins -----------------------------
constexpr uint8_t STEP_1_PIN = 25;
constexpr uint8_t STEP_2_PIN = 14;
constexpr uint8_t DIR_PIN = 26;
constexpr uint8_t ENABLE_PIN = 27;  // TMC ENN: LOW=enabled, HIGH=disabled.
constexpr uint8_t TMC_UART_RX_PIN = 16;
constexpr uint8_t TMC_UART_TX_PIN = 17;
constexpr uint8_t DIAG_1_PIN = 39;
constexpr uint8_t DIAG_2_PIN = 19;
constexpr uint8_t LIMIT_1_PIN = 34;
constexpr uint8_t LIMIT_2_PIN = 35;
constexpr uint8_t VREF_1_ADC_PIN = 32;
constexpr uint8_t VREF_2_ADC_PIN = 33;

// BTT V1.3 uses 0.110 ohm external current-sense resistors.
constexpr float R_SENSE_OHMS = 0.110f;
constexpr uint8_t TMC_1_ADDRESS = 0b00;
constexpr uint8_t TMC_2_ADDRESS = 0b01;

// Conservative initial settings. Set RUN_CURRENT_MA for the motor's rating,
// driver cooling, and mechanics before increasing it.
constexpr uint16_t DEFAULT_RUN_CURRENT_MA = 600;
constexpr uint8_t DEFAULT_HOLD_PERCENT = 100;
constexpr uint8_t DEFAULT_IHOLD_DELAY = 8;   // 0..15: current ramp-down rate.
constexpr uint8_t DEFAULT_TPOWERDOWN = 20;   // ~0.44 s at the nominal 12 MHz clock.
constexpr uint8_t DEFAULT_SG_THRESHOLD = 0;  // Least sensitive; tune from logs.
constexpr float DEFAULT_SPEED_STEPS_S = 200.0f;
constexpr float DEFAULT_ACCEL_STEPS_S2 = 400.0f;
constexpr uint32_t DEFAULT_PRECHARGE_MS = 750;
constexpr uint32_t TELEMETRY_PERIOD_MS = 250;

HardwareSerial TMCSerial(2);
TMC2209Stepper tmc1(&TMCSerial, R_SENSE_OHMS, TMC_1_ADDRESS);
TMC2209Stepper tmc2(&TMCSerial, R_SENSE_OHMS, TMC_2_ADDRESS);
AccelStepper motor1(AccelStepper::DRIVER, STEP_1_PIN, DIR_PIN);
AccelStepper motor2(AccelStepper::DRIVER, STEP_2_PIN, DIR_PIN);

enum MotorSelection : uint8_t { MOTOR_1 = 1, MOTOR_2 = 2, BOTH_MOTORS = 3 };
MotorSelection selectedMotors = BOTH_MOTORS;

uint16_t requestedRunCurrentMa = DEFAULT_RUN_CURRENT_MA;
uint8_t requestedHoldPercent = DEFAULT_HOLD_PERCENT;
uint8_t requestedIholdDelay = DEFAULT_IHOLD_DELAY;
uint8_t requestedPowerDown = DEFAULT_TPOWERDOWN;
uint8_t requestedSgThreshold = DEFAULT_SG_THRESHOLD;
float requestedSpeed = DEFAULT_SPEED_STEPS_S;
float requestedAcceleration = DEFAULT_ACCEL_STEPS_S2;
uint32_t prechargeMs = DEFAULT_PRECHARGE_MS;
bool outputsEnabled = false;
bool telemetryEnabled = true;
bool coolStepEnabled = false;
bool motionArmed = false;
uint32_t motionStartAtMs = 0;
long pendingSteps = 0;
uint32_t lastTelemetryMs = 0;

String inputLine;

bool selected(uint8_t motorNumber) {
  return (selectedMotors & motorNumber) != 0;
}

bool anyLimitOpen() {
  // NC wiring: LOW is healthy; HIGH means pressed, disconnected, or broken wire.
  return digitalRead(LIMIT_1_PIN) == HIGH || digitalRead(LIMIT_2_PIN) == HIGH;
}

void setOutputsEnabled(bool enable) {
  digitalWrite(ENABLE_PIN, enable ? LOW : HIGH);
  outputsEnabled = enable;
  Serial.printf("EN=%s (GPIO27=%s)\n", enable ? "enabled" : "disabled",
                enable ? "LOW" : "HIGH");
}

void stopMotion(const char *reason) {
  motor1.stop();
  motor2.stop();
  motionArmed = false;
  pendingSteps = 0;
  Serial.printf("STOP: %s\n", reason);
}

void hardStopAndDisable(const char *reason) {
  // setCurrentPosition() cancels an AccelStepper target immediately. This is
  // intentionally abrupt for a limit/fault diagnostic stop.
  motor1.setCurrentPosition(motor1.currentPosition());
  motor2.setCurrentPosition(motor2.currentPosition());
  motionArmed = false;
  pendingSteps = 0;
  setOutputsEnabled(false);
  Serial.printf("ABORT: %s\n", reason);
}

float vrefVolts(uint8_t pin) {
  // analogReadMilliVolts() uses the ESP32 Arduino calibration when available.
  // Average 32 readings because the ESP32 ADC and nearby chopper are noisy.
  uint32_t sumMv = 0;
  for (uint8_t i = 0; i < 32; ++i) sumMv += analogReadMilliVolts(pin);
  return (sumMv / 32.0f) / 1000.0f;
}

float standaloneRmsFromVref(float volts) {
  // Exact datasheet equation for Rsense=0.110 ohm and analog full scale:
  // Irms = 0.325/(Rsense+0.020) / sqrt(2) * Vref/2.5
  return (0.325f / (R_SENSE_OHMS + 0.020f)) * 0.70710678f * (volts / 2.5f);
}

void applyCurrentSettings(TMC2209Stepper &driver) {
  // Disable analog scaling so rms_current() is controlled predictably by UART.
  // VREF can still be logged, but turning the pot will no longer set current.
  driver.I_scale_analog(false);
  driver.rms_current(requestedRunCurrentMa, requestedHoldPercent / 100.0f);
  driver.iholddelay(requestedIholdDelay);
  driver.TPOWERDOWN(requestedPowerDown);
}

void applyMotionSettings() {
  motor1.setMaxSpeed(requestedSpeed);
  motor2.setMaxSpeed(requestedSpeed);
  motor1.setAcceleration(requestedAcceleration);
  motor2.setAcceleration(requestedAcceleration);
}

void setCoolStep(TMC2209Stepper &driver, bool enable) {
  if (enable) {
    // Starting values from the TMCStepper StallGuard example. CoolStep varies
    // CS_ACTUAL between a fraction of IRUN and IRUN; it never exceeds IRUN.
    driver.TCOOLTHRS(0xFFFFF); // Permit StallGuard/CoolStep over test speeds.
    driver.semin(5);           // Nonzero enables CoolStep.
    driver.semax(2);           // Hysteresis above SEMIN.
    driver.seup(0b01);         // Current rises by 2 scale steps when loaded.
    driver.sedn(0b01);         // Current drops after 8 high-headroom readings.
    driver.seimin(false);      // Minimum is 1/2 IRUN, not 1/4.
  } else {
    driver.semin(0);           // TMC2209 definition: zero disables CoolStep.
  }
}

void configureDriver(TMC2209Stepper &driver) {
  driver.begin();
  driver.toff(4);              // Nonzero is required; zero disables outputs.
  driver.blank_time(24);
  driver.microsteps(16);
  driver.intpol(true);         // Internally interpolate STEP/DIR to 256 usteps.
  driver.en_spreadCycle(false);// StallGuard4 on 2209 is designed for StealthChop.
  driver.pwm_autoscale(true);
  driver.pwm_autograd(true);
  applyCurrentSettings(driver);
  driver.TCOOLTHRS(0xFFFFF);
  driver.SGTHRS(requestedSgThreshold);
  setCoolStep(driver, false);
  driver.GSTAT(0b111);         // Clear reset/driver-error/charge-pump flags.
}

void printConnection(const char *name, TMC2209Stepper &driver) {
  uint8_t result = driver.test_connection();
  Serial.printf("%s UART: %s", name, result == 0 ? "OK" : "FAILED");
  if (result == 1) Serial.print(" (no/invalid reply; check bus/address/MS straps)");
  if (result == 2) Serial.print(" (all-zero reply; check VIO/VM/GND)");
  Serial.printf(", IC version=0x%02X\n", driver.version());
}

void printOneTelemetry(const char *name, TMC2209Stepper &driver,
                       uint8_t diagPin, uint8_t vrefPin) {
  // Parse the single DRV_STATUS reply directly. Calling each convenience
  // accessor separately would create many UART transactions and could disturb
  // time-sensitive STEP generation.
  const uint32_t drv = driver.DRV_STATUS();
  const uint32_t ioin = driver.IOIN();
  const uint16_t sg = driver.SG_RESULT();
  const uint8_t cs = (drv >> 16) & 0x1F;
  const uint16_t estimatedMa = driver.cs2rms(cs);
  const float vref = vrefVolts(vrefPin);

  Serial.printf(
    "%s VREF=%.3fV (~%.0fmA standalone) SG=%u DIAG=%u ENN=%u CS=%u (~%umA RMS) "
    "standstill=%u otpw=%u ot=%u s2ga=%u s2gb=%u ola=%u olb=%u\n",
    name, vref, standaloneRmsFromVref(vref) * 1000.0f, sg,
    digitalRead(diagPin), ioin & 1, cs, estimatedMa, (drv >> 31) & 1, drv & 1,
    (drv >> 1) & 1, (drv >> 2) & 1, (drv >> 3) & 1,
    (drv >> 6) & 1, (drv >> 7) & 1);
}

void printTelemetry() {
  Serial.printf("state EN=%u moving1=%u moving2=%u limits=%u/%u coolstep=%u | ",
                outputsEnabled, motor1.isRunning(), motor2.isRunning(),
                digitalRead(LIMIT_1_PIN), digitalRead(LIMIT_2_PIN),
                coolStepEnabled);
  printOneTelemetry("V1", tmc1, DIAG_1_PIN, VREF_1_ADC_PIN);
  Serial.print("                                                        | ");
  printOneTelemetry("V2", tmc2, DIAG_2_PIN, VREF_2_ADC_PIN);
}

void printSettings() {
  Serial.printf(
    "selection=%u run=%umA hold=%u%% iholddelay=%u powerdown=%u "
    "precharge=%lums speed=%.1f accel=%.1f SGTHRS=%u coolstep=%u\n",
    selectedMotors, requestedRunCurrentMa, requestedHoldPercent,
    requestedIholdDelay, requestedPowerDown, (unsigned long)prechargeMs,
    requestedSpeed, requestedAcceleration, requestedSgThreshold,
    coolStepEnabled);
}

void printHelp() {
  Serial.println(R"HELP(
Commands (send with Newline):
  help                 show this list
  status               print settings and one telemetry sample
  log on|off            continuous telemetry every 250 ms
  enable 1|0            energize or float all drivers (global EN)
  motor 1|2|both        select which vertical motor receives future moves
  move <steps>          relative move; negative reverses direction
  stop                  decelerated stop
  estop                 immediate stop and disable
  speed <steps/s>       set maximum speed (10..5000)
  accel <steps/s^2>     set acceleration (10..20000)
  precharge <ms>        enabled stationary dwell before first STEP (0..5000)
  run <mA>              set UART RMS run current (100..1800)
  hold <percent>        hold current as 0..100 percent of run current
  holddelay <0..15>     rate of transition from IRUN to IHOLD
  powerdown <0..255>    delay before hold-current transition (~0..5.6 s)
  sgthrs <0..255>       higher means more sensitive stall/DIAG threshold
  coolstep on|off       load-adaptive current, never greater than IRUN
  mode stealth|spread   StealthChop for SG4; SpreadCycle for robust torque
  clear                 clear driver status flags

Interpretation:
  - Enabled but limp: verify GPIO27 is LOW, UART OK, TOFF nonzero, CS nonzero,
    no OT/short flags, correct coil pairs, and motor-suitable current.
  - Failure only at the first step: raise precharge, lower initial speed, and
    lower acceleration. Holding current cannot fix a motor that never energizes.
  - SG_RESULT is meaningful while moving: lower = more load/closer to stall.
    Compare at the same speed/current/direction; it is not an absolute percent.
)HELP");
}

void queueMove(long steps) {
  if (steps == 0) return;
  if (anyLimitOpen()) {
    Serial.println("Move refused: a vertical limit input is HIGH/open.");
    return;
  }
  setOutputsEnabled(true);
  pendingSteps = steps;
  motionStartAtMs = millis() + prechargeMs;
  motionArmed = true;
  Serial.printf("Move armed: %ld steps after %lu ms stationary precharge.\n",
                steps, (unsigned long)prechargeMs);
}

long argumentAsLong(const String &line) {
  int separator = line.indexOf(' ');
  return separator < 0 ? 0 : line.substring(separator + 1).toInt();
}

void applyCurrentToBoth() {
  applyCurrentSettings(tmc1);
  applyCurrentSettings(tmc2);
  Serial.println("Current/hold timing applied to both vertical drivers.");
  printSettings();
}

void processCommand(String line) {
  line.trim();
  line.toLowerCase();
  if (line.length() == 0) return;

  if (line == "help") printHelp();
  else if (line == "status") { printSettings(); printTelemetry(); }
  else if (line == "log on") { telemetryEnabled = true; Serial.println("Telemetry ON"); }
  else if (line == "log off") { telemetryEnabled = false; Serial.println("Telemetry OFF"); }
  else if (line == "enable 1") setOutputsEnabled(true);
  else if (line == "enable 0") { stopMotion("disabled by command"); setOutputsEnabled(false); }
  else if (line == "motor 1") { selectedMotors = MOTOR_1; printSettings(); }
  else if (line == "motor 2") { selectedMotors = MOTOR_2; printSettings(); }
  else if (line == "motor both") { selectedMotors = BOTH_MOTORS; printSettings(); }
  else if (line.startsWith("move ")) queueMove(argumentAsLong(line));
  else if (line == "stop") stopMotion("operator command");
  else if (line == "estop") hardStopAndDisable("operator command");
  else if (line.startsWith("speed ")) {
    float value = line.substring(6).toFloat();
    if (value < 10 || value > 5000) Serial.println("ERROR: speed range is 10..5000");
    else { requestedSpeed = value; applyMotionSettings(); printSettings(); }
  }
  else if (line.startsWith("accel ")) {
    float value = line.substring(6).toFloat();
    if (value < 10 || value > 20000) Serial.println("ERROR: accel range is 10..20000");
    else { requestedAcceleration = value; applyMotionSettings(); printSettings(); }
  }
  else if (line.startsWith("precharge ")) {
    long value = argumentAsLong(line);
    if (value < 0 || value > 5000) Serial.println("ERROR: precharge range is 0..5000 ms");
    else { prechargeMs = value; printSettings(); }
  }
  else if (line.startsWith("run ")) {
    long value = argumentAsLong(line);
    if (value < 100 || value > 1800) Serial.println("ERROR: run range is 100..1800 mA RMS");
    else { requestedRunCurrentMa = value; applyCurrentToBoth(); }
  }
  else if (line.startsWith("hold ")) {
    long value = argumentAsLong(line);
    if (value < 0 || value > 100) Serial.println("ERROR: hold range is 0..100 percent");
    else { requestedHoldPercent = value; applyCurrentToBoth(); }
  }
  else if (line.startsWith("holddelay ")) {
    long value = argumentAsLong(line);
    if (value < 0 || value > 15) Serial.println("ERROR: holddelay range is 0..15");
    else { requestedIholdDelay = value; applyCurrentToBoth(); }
  }
  else if (line.startsWith("powerdown ")) {
    long value = argumentAsLong(line);
    if (value < 0 || value > 255) Serial.println("ERROR: powerdown range is 0..255");
    else { requestedPowerDown = value; applyCurrentToBoth(); }
  }
  else if (line.startsWith("sgthrs ")) {
    long value = argumentAsLong(line);
    if (value < 0 || value > 255) Serial.println("ERROR: sgthrs range is 0..255");
    else {
      requestedSgThreshold = value;
      tmc1.SGTHRS(value); tmc2.SGTHRS(value);
      printSettings();
    }
  }
  else if (line == "coolstep on" || line == "coolstep off") {
    coolStepEnabled = line.endsWith("on");
    setCoolStep(tmc1, coolStepEnabled);
    setCoolStep(tmc2, coolStepEnabled);
    printSettings();
  }
  else if (line == "mode stealth" || line == "mode spread") {
    bool spread = line.endsWith("spread");
    tmc1.en_spreadCycle(spread); tmc2.en_spreadCycle(spread);
    if (spread && coolStepEnabled) {
      coolStepEnabled = false;
      setCoolStep(tmc1, false); setCoolStep(tmc2, false);
      Serial.println("CoolStep disabled for this diagnostic in SpreadCycle.");
    }
    Serial.println(spread ? "SpreadCycle selected." : "StealthChop selected.");
  }
  else if (line == "clear") {
    tmc1.GSTAT(0b111); tmc2.GSTAT(0b111);
    Serial.println("GSTAT flags cleared.");
  }
  else Serial.println("Unknown command. Type: help");
}

void serviceSerial() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      if (inputLine.length()) { processCommand(inputLine); inputLine = ""; }
    } else if (inputLine.length() < 80) inputLine += c;
  }
}

void serviceMotion() {
  if (anyLimitOpen() && (motionArmed || motor1.isRunning() || motor2.isRunning())) {
    hardStopAndDisable("vertical limit opened");
    return;
  }

  if (motionArmed && (int32_t)(millis() - motionStartAtMs) >= 0) {
    motionArmed = false;
    if (selected(MOTOR_1)) motor1.move(pendingSteps);
    if (selected(MOTOR_2)) motor2.move(pendingSteps);
    Serial.println("Precharge complete; STEP pulses starting.");
    pendingSteps = 0;
  }

  // Both motors use the same DIR pin. This diagnostic commands them in the
  // same direction; never give the two AccelStepper instances opposing moves.
  motor1.run();
  motor2.run();
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\nVertical TMC2209 UART diagnostic starting...");

  pinMode(ENABLE_PIN, OUTPUT);
  pinMode(STEP_1_PIN, OUTPUT);
  pinMode(STEP_2_PIN, OUTPUT);
  pinMode(DIR_PIN, OUTPUT);
  pinMode(DIAG_1_PIN, INPUT);
  pinMode(DIAG_2_PIN, INPUT);
  pinMode(LIMIT_1_PIN, INPUT); // GPIO34/35 have no internal pull-ups.
  pinMode(LIMIT_2_PIN, INPUT);
  digitalWrite(STEP_1_PIN, LOW);
  digitalWrite(STEP_2_PIN, LOW);
  setOutputsEnabled(false);

  analogReadResolution(12);
  analogSetPinAttenuation(VREF_1_ADC_PIN, ADC_11db);
  analogSetPinAttenuation(VREF_2_ADC_PIN, ADC_11db);

  motor1.setMinPulseWidth(2);
  motor2.setMinPulseWidth(2);
  applyMotionSettings();

  // The BTT V1.3 modules use a one-wire half-duplex bus. ESP32 RX joins the
  // bus directly; TX joins through about 1k. Module side pins labelled TX stay
  // disconnected and R10 remains open.
  TMCSerial.begin(115200, SERIAL_8N1, TMC_UART_RX_PIN, TMC_UART_TX_PIN);
  configureDriver(tmc1);
  configureDriver(tmc2);
  printConnection("Vertical 1/address 0", tmc1);
  printConnection("Vertical 2/address 1", tmc2);

  if (anyLimitOpen()) {
    Serial.println("WARNING: a vertical NC limit input is HIGH/open; moves are blocked.");
  }
  printSettings();
  printHelp();
}

void loop() {
  serviceSerial();
  serviceMotion();

  if (telemetryEnabled && millis() - lastTelemetryMs >= TELEMETRY_PERIOD_MS) {
    lastTelemetryMs = millis();
    printTelemetry();
  }
}
