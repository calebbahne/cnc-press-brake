/*
  09.12_vertical_motor_1_uart_config_test.ino

  Second-stage bring-up test for vertical motor 1's BIGTREETECH TMC2209 V1.3.
  The earlier UART-only test proved two-way communication. This sketch goes one
  step further: it writes a conservative driver configuration over UART, reads
  the registers back, and reports whether the important settings match.

  This sketch deliberately sends no STEP pulses and keeps GPIO27 HIGH, so the
  shared global enable leaves every driver output disabled. DIAG, StallGuard,
  limit switches, and the other three driver addresses are not used.

  REQUIRED POWER AND WIRING
    - 24 V motor power must be ON; VIO alone does not power the TMC2209 core.
    - ESP32 GPIO17 TX -> about 1 kOhm -> driver RX/PDN_UART
    - ESP32 GPIO16 RX ----------------> driver RX/PDN_UART
    - Driver side pin marked TX remains disconnected; R10 remains open.
    - Driver MS2=GND and MS1=GND select UART address 0.
    - Driver VIO=ESP32 3V3 and all grounds are common.

  Open Serial Monitor at 115200 baud with Newline or Both NL & CR selected.
  Commands:
    status  - read and print the current configuration
    apply   - write the test configuration again and verify it
    help    - print the command list

  A passing test proves UART register writes and reads. It does not yet prove
  motor current or motion because the global driver enable remains inactive.
*/

#include <Arduino.h>
#include <TMCStepper.h>

constexpr uint8_t ENABLE_PIN = 27;
constexpr uint8_t TMC_UART_RX_PIN = 16;
constexpr uint8_t TMC_UART_TX_PIN = 17;
constexpr uint8_t TMC_ADDRESS = 0;
constexpr float R_SENSE_OHMS = 0.110f;

constexpr uint16_t RUN_CURRENT_MA = 600;
constexpr float HOLD_CURRENT_MULTIPLIER = 0.50f;
constexpr uint16_t MICROSTEPS = 16;
constexpr uint8_t TOFF_VALUE = 4;
constexpr uint8_t BLANK_TIME_CLOCKS = 24;
constexpr uint8_t IHOLD_DELAY = 8;
constexpr uint8_t POWER_DOWN_DELAY = 20;
constexpr uint32_t STATUS_INTERVAL_MS = 3000;

HardwareSerial TMCSerial(2);
TMC2209Stepper driver(&TMCSerial, R_SENSE_OHMS, TMC_ADDRESS);

String inputLine;
uint32_t lastStatusMs = 0;
bool uartReady = false;
bool lastWritesAccepted = false;

bool connectionOkay() {
  const uint8_t result = driver.test_connection();
  if (result == 0) return true;

  Serial.printf("ERROR UART connection test returned %u. Confirm that 24 V is ON, "
                "then check VIO, common ground, RX bus wiring, and address 0.\n",
                result);
  return false;
}

void printStatus() {
  if (!connectionOkay()) {
    uartReady = false;
    return;
  }

  uartReady = true;
  const uint32_t ioin = driver.IOIN();
  const uint32_t gconf = driver.GCONF();
  const uint32_t iholdIrun = driver.IHOLD_IRUN();
  const uint32_t chopconf = driver.CHOPCONF();
  const uint32_t pwmconf = driver.PWMCONF();
  const uint32_t drvStatus = driver.DRV_STATUS();

  Serial.printf(
    "STATUS address=%u version=0x%02X IFCNT=%u ENN=%u standstill=%u\n",
    TMC_ADDRESS, static_cast<unsigned>((ioin >> 24) & 0xFF), driver.IFCNT(),
    static_cast<unsigned>(ioin & 1),
    static_cast<unsigned>((drvStatus >> 31) & 1));
  Serial.printf("  GCONF=0x%08lX IHOLD_IRUN(cached)=0x%08lX\n",
                static_cast<unsigned long>(gconf),
                static_cast<unsigned long>(iholdIrun));
  Serial.printf("  CHOPCONF=0x%08lX PWMCONF=0x%08lX DRV_STATUS=0x%08lX\n",
                static_cast<unsigned long>(chopconf),
                static_cast<unsigned long>(pwmconf),
                static_cast<unsigned long>(drvStatus));
  Serial.printf(
    "  decoded/cached: run~%umA hold~%umA microsteps=%u toff=%u blank=%u "
    "intpol=%u spreadCycle=%u analogScale=%u pwmAutoscale=%u pwmAutograd=%u\n",
    driver.cs2rms(driver.irun()), driver.cs2rms(driver.ihold()),
    driver.microsteps(), driver.toff(), driver.blank_time(), driver.intpol(),
    driver.en_spreadCycle(), driver.I_scale_analog(), driver.pwm_autoscale(),
    driver.pwm_autograd());
}

bool settingsMatch() {
  const bool match =
    driver.microsteps() == MICROSTEPS &&
    driver.toff() == TOFF_VALUE &&
    driver.blank_time() == BLANK_TIME_CLOCKS &&
    driver.intpol() &&
    !driver.en_spreadCycle() &&
    !driver.I_scale_analog() &&
    driver.pwm_autoscale() &&
    driver.pwm_autograd() &&
    driver.iholddelay() == IHOLD_DELAY &&
    driver.TPOWERDOWN() == POWER_DOWN_DELAY;

  const bool pass = lastWritesAccepted && match;
  Serial.println(pass
    ? "CONFIG PASS: IFCNT confirms accepted writes and all checked settings match."
    : "CONFIG FAIL: writes were not confirmed or one or more settings differ.");
  return pass;
}

void applyConfiguration() {
  if (!connectionOkay()) {
    uartReady = false;
    Serial.println("CONFIG NOT APPLIED.");
    return;
  }

  uartReady = true;
  const uint8_t ifcntBefore = driver.IFCNT();

  driver.pdn_disable(true);
  driver.mstep_reg_select(true);
  driver.I_scale_analog(false);
  driver.rms_current(RUN_CURRENT_MA, HOLD_CURRENT_MULTIPLIER);
  driver.iholddelay(IHOLD_DELAY);
  driver.TPOWERDOWN(POWER_DOWN_DELAY);
  driver.toff(TOFF_VALUE);
  driver.blank_time(BLANK_TIME_CLOCKS);
  driver.microsteps(MICROSTEPS);
  driver.intpol(true);
  driver.en_spreadCycle(false);
  driver.pwm_autoscale(true);
  driver.pwm_autograd(true);

  const uint8_t ifcntAfter = driver.IFCNT();
  const uint8_t acceptedWriteCount =
    static_cast<uint8_t>(ifcntAfter - ifcntBefore);  // Handles 8-bit wraparound.
  lastWritesAccepted = acceptedWriteCount > 0;
  Serial.printf("ACK configuration sent; IFCNT %u -> %u (%u writes accepted).\n",
                ifcntBefore, ifcntAfter, acceptedWriteCount);
  printStatus();
  settingsMatch();
}

void printHelp() {
  Serial.println("Commands: status | apply | help");
  Serial.println("GPIO27 remains HIGH: all motor outputs stay disabled.");
}

void processCommand(String line) {
  line.trim();
  line.toLowerCase();
  if (line.length() == 0) return;

  if (line == "status") printStatus();
  else if (line == "apply") applyConfiguration();
  else if (line == "help") printHelp();
  else Serial.println("ERROR unknown command. Type: help");
}

void serviceSerial() {
  while (Serial.available()) {
    const char c = static_cast<char>(Serial.read());
    if (c == '\n' || c == '\r') {
      if (inputLine.length() > 0) {
        processCommand(inputLine);
        inputLine = "";
      }
    } else if (inputLine.length() < 40) {
      inputLine += c;
    }
  }
}

void setup() {
  Serial.begin(115200);

  pinMode(ENABLE_PIN, OUTPUT);
  digitalWrite(ENABLE_PIN, HIGH);  // Global EN is active-low; stay disabled.

  delay(1000);
  Serial.println("\nVertical motor 1 TMC2209 UART configuration test");
  Serial.println("24 V must be ON. No STEP pulses will be sent.");
  Serial.println("All driver outputs remain disabled by global GPIO27.");

  TMCSerial.begin(115200, SERIAL_8N1, TMC_UART_RX_PIN, TMC_UART_TX_PIN);
  driver.begin();
  applyConfiguration();
  printHelp();
  lastStatusMs = millis();
}

void loop() {
  serviceSerial();

  if (millis() - lastStatusMs >= STATUS_INTERVAL_MS) {
    lastStatusMs = millis();
    printStatus();
    if (!uartReady) {
      digitalWrite(ENABLE_PIN, HIGH);
    }
  }
}
