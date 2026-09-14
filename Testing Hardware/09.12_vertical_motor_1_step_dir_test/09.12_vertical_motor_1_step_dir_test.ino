/*
  09.12_vertical_motor_1_step_dir_test.ino

  Minimum STEP/DIR test for vertical motor 1 and its TMC2209 driver.

  Wiring from electrical-quick-reference.md:
    ESP32 GPIO25 -> vertical motor 1 driver STEP
    ESP32 GPIO26 -> vertical drivers DIR
    ESP32 GPIO27 -> global driver EN (active LOW)

  UART and DIAG may remain physically connected, but this sketch does not
  initialize, read, configure, or otherwise use them.

  After uploading, fully remove both 24 V motor power and USB/controller power,
  then restore them. This cold-starts the TMC2209 so UART register settings from
  an earlier sketch cannot carry into this standalone STEP/DIR test.

  CAUTION: GPIO27 enables all four drivers. Keep the mechanism clear and be
  ready to remove motor power. This isolated test does not check limits.
*/

#include <Arduino.h>

constexpr uint8_t STEP_PIN = 14; // 25 stepper 1
constexpr uint8_t DIR_PIN = 26; // 26 stepper 1
constexpr uint8_t ENABLE_PIN = 27;

constexpr uint32_t STEPS_PER_MOVE = 8000;
constexpr uint32_t STEP_HALF_PERIOD_US = 1500;  // About 333 STEP pulses/second.
constexpr uint32_t PAUSE_MS = 500;

void moveMotor(uint32_t steps, bool direction) {
  digitalWrite(DIR_PIN, direction);
  delayMicroseconds(10);  // DIR setup time before the first STEP edge.

  for (uint32_t step = 0; step < steps; ++step) {
    digitalWrite(STEP_PIN, HIGH);
    delayMicroseconds(STEP_HALF_PERIOD_US);
    digitalWrite(STEP_PIN, LOW);
    delayMicroseconds(STEP_HALF_PERIOD_US);
  }
}

void setup() {
  pinMode(STEP_PIN, OUTPUT);
  pinMode(DIR_PIN, OUTPUT);
  pinMode(ENABLE_PIN, OUTPUT);

  digitalWrite(STEP_PIN, LOW);
  digitalWrite(DIR_PIN, LOW);
  digitalWrite(ENABLE_PIN, HIGH);  // Start disabled; EN is active LOW.

  delay(1000);
  digitalWrite(ENABLE_PIN, LOW);   // Enables every driver on the shared EN line.
  delay(500);                      // Let motor current settle before stepping.
}

void loop() {
  moveMotor(STEPS_PER_MOVE, HIGH);
  delay(PAUSE_MS);

  moveMotor(STEPS_PER_MOVE, LOW);
  delay(PAUSE_MS);
}
