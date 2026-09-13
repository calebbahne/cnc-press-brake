/*
  09.12_vertical_motor_1_uart_test.ino

  Minimum UART communication test for vertical motor 1's BTT TMC2209 V1.3.
  This sketch sends no STEP pulses and does not use DIAG or StallGuard.

  Required Arduino library:
    TMCStepper by teemuatlut

  Wiring from electrical-quick-reference.md:
    ESP32 GPIO17 TX -> about 1 kOhm -> driver RX/PDN_UART
    ESP32 GPIO16 RX ----------------> driver RX/PDN_UART
    Driver side pin marked TX is left disconnected; R10 remains open.
    Driver MS2 = GND and MS1 = GND, selecting UART address 0.
    Driver VIO = ESP32 3V3 and all grounds are common.

  Open Serial Monitor at 115200 baud. A successful two-way UART link prints
  "UART PASS" repeatedly. Result 1 usually means no/bad reply; result 2 means
  an all-zero reply. The driver stays disabled throughout this test.
*/

#include <Arduino.h>
#include <TMCStepper.h>

constexpr uint8_t ENABLE_PIN = 27;
constexpr uint8_t TMC_UART_RX_PIN = 16;
constexpr uint8_t TMC_UART_TX_PIN = 17;
constexpr uint8_t TMC_ADDRESS = 0;
constexpr float R_SENSE_OHMS = 0.110f;
constexpr uint32_t REPORT_INTERVAL_MS = 2000;

HardwareSerial TMCSerial(2);
TMC2209Stepper driver(&TMCSerial, R_SENSE_OHMS, TMC_ADDRESS);

uint32_t lastReportMs = 0;

void reportUart() {
  const uint8_t result = driver.test_connection();

  if (result == 0) {
    Serial.printf("UART PASS: address=%u, IOIN=0x%08lX, GSTAT=0x%02X\n",
                  TMC_ADDRESS,
                  static_cast<unsigned long>(driver.IOIN()),
                  driver.GSTAT());
  } else if (result == 1) {
    Serial.println("UART FAIL (result 1): no valid reply; check shared bus wiring and address straps.");
  } else if (result == 2) {
    Serial.println("UART FAIL (result 2): all-zero reply; check VIO, ground, bus wiring, and address.");
  } else {
    Serial.printf("UART FAIL (unexpected result %u).\n", result);
  }
}

void setup() {
  Serial.begin(115200);

  pinMode(ENABLE_PIN, OUTPUT);
  digitalWrite(ENABLE_PIN, HIGH);  // Keep every driver disabled; EN is active LOW.

  delay(1000);
  Serial.println("\nVertical motor 1 TMC2209 UART-only test");
  Serial.println("Motor outputs remain disabled; DIAG and StallGuard are unused.");

  TMCSerial.begin(115200, SERIAL_8N1, TMC_UART_RX_PIN, TMC_UART_TX_PIN);
  driver.begin();
  reportUart();
  lastReportMs = millis();
}

void loop() {
  if (millis() - lastReportMs >= REPORT_INTERVAL_MS) {
    lastReportMs = millis();
    reportUart();
  }
}
