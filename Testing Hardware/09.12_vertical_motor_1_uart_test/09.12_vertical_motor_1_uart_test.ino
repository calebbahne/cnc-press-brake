/*
  09.12_vertical_motor_1_uart_test.ino

  UART communication test for all four BTT TMC2209 V1.3 drivers.
  This sketch sends no STEP pulses and does not use DIAG or StallGuard.

  Required Arduino library:
    TMCStepper by teemuatlut

  Wiring from electrical-quick-reference.md:
    ESP32 GPIO17 TX -> about 1 kOhm -> driver RX/PDN_UART
    ESP32 GPIO16 RX ----------------> driver RX/PDN_UART
    Driver side pin marked TX is left disconnected; R10 remains open.
    Vertical driver 1: MS2 = GND, MS1 = GND, selecting UART address 0.
    Horizontal driver 1: MS2 = 3V3/VIO, MS1 = GND, selecting UART address 1.
    Vertical driver 2: MS2 = GND, MS1 = 3V3/VIO, selecting UART address 2.
    Horizontal driver 2: MS2 = 3V3/VIO, MS1 = 3V3/VIO, selecting UART address 3.
    Driver VIO = ESP32 3V3 and all grounds are common.

  Open Serial Monitor at 115200 baud. A successful two-way UART link prints
  "UART PASS" repeatedly. Result 1 usually means no/bad reply; result 2 means
  an all-zero reply. The drivers stay disabled throughout this test.
*/

#include <Arduino.h>
#include <TMCStepper.h>

constexpr uint8_t ENABLE_PIN = 27;  // Global driver enable, active LOW.
constexpr uint8_t TMC_UART_RX_PIN = 16;
constexpr uint8_t TMC_UART_TX_PIN = 17;

// Set any of these to false to skip that driver during the UART test.
constexpr bool CHECK_VERTICAL_1 = true;
constexpr bool CHECK_HORIZONTAL_1 = true;
constexpr bool CHECK_VERTICAL_2 = true;
constexpr bool CHECK_HORIZONTAL_2 = true;

constexpr uint8_t VERTICAL_1_ADDRESS = 0;
constexpr uint8_t HORIZONTAL_1_ADDRESS = 1;
constexpr uint8_t VERTICAL_2_ADDRESS = 2;
constexpr uint8_t HORIZONTAL_2_ADDRESS = 3;
constexpr float R_SENSE_OHMS = 0.110f;
constexpr uint32_t REPORT_INTERVAL_MS = 2000;

HardwareSerial TMCSerial(2);
TMC2209Stepper verticalDriver1(&TMCSerial, R_SENSE_OHMS, VERTICAL_1_ADDRESS);
TMC2209Stepper horizontalDriver1(&TMCSerial, R_SENSE_OHMS, HORIZONTAL_1_ADDRESS);
TMC2209Stepper verticalDriver2(&TMCSerial, R_SENSE_OHMS, VERTICAL_2_ADDRESS);
TMC2209Stepper horizontalDriver2(&TMCSerial, R_SENSE_OHMS, HORIZONTAL_2_ADDRESS);

uint32_t lastReportMs = 0;

void reportDriverUart(const char *driverName,
                      uint8_t address,
                      TMC2209Stepper &driver) {
  const uint8_t result = driver.test_connection();

  if (result == 0) {
    Serial.printf("%s UART PASS: address=%u, IOIN=0x%08lX, GSTAT=0x%02X\n",
                  driverName,
                  address,
                  static_cast<unsigned long>(driver.IOIN()),
                  driver.GSTAT());
  } else if (result == 1) {
    Serial.printf("%s UART FAIL at address %u (result 1): no valid reply; check shared bus wiring and address straps.\n",
                  driverName, address);
  } else if (result == 2) {
    Serial.printf("%s UART FAIL at address %u (result 2): all-zero reply; check VIO, ground, bus wiring, and address.\n",
                  driverName, address);
  } else {
    Serial.printf("%s UART FAIL at address %u (unexpected result %u).\n",
                  driverName, address, result);
  }
}

void reportUart() {
  if (CHECK_VERTICAL_1) {
    reportDriverUart("Vertical motor 1", VERTICAL_1_ADDRESS, verticalDriver1);
  }
  if (CHECK_HORIZONTAL_1) {
    reportDriverUart("Horizontal motor 1", HORIZONTAL_1_ADDRESS, horizontalDriver1);
  }
  if (CHECK_VERTICAL_2) {
    reportDriverUart("Vertical motor 2", VERTICAL_2_ADDRESS, verticalDriver2);
  }
  if (CHECK_HORIZONTAL_2) {
    reportDriverUart("Horizontal motor 2", HORIZONTAL_2_ADDRESS, horizontalDriver2);
  }
}

void setup() {
  Serial.begin(115200);

  pinMode(ENABLE_PIN, OUTPUT);
  digitalWrite(ENABLE_PIN, HIGH);  // Keep every driver disabled; EN is active LOW.

  delay(1000);
  Serial.println("\nFour-driver TMC2209 UART-only test");
  Serial.println("Motor outputs remain disabled; DIAG and StallGuard are unused.");

  TMCSerial.begin(115200, SERIAL_8N1, TMC_UART_RX_PIN, TMC_UART_TX_PIN);
  if (CHECK_VERTICAL_1) verticalDriver1.begin();
  if (CHECK_HORIZONTAL_1) horizontalDriver1.begin();
  if (CHECK_VERTICAL_2) verticalDriver2.begin();
  if (CHECK_HORIZONTAL_2) horizontalDriver2.begin();
  reportUart();
  lastReportMs = millis();
}

void loop() {
  if (millis() - lastReportMs >= REPORT_INTERVAL_MS) {
    lastReportMs = millis();
    reportUart();
  }
}
