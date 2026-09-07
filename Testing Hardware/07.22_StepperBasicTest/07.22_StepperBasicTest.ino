/*
  Basic open-loop vertical-stepper test for the current ESP32 controller.

  GPIO25 STEP and GPIO26 DIR are shared by both vertical TMC2209 drivers.
  GPIO27 is the global TMC enable and is active-low. This sketch immediately
  enables the drivers and repeatedly moves the vertical stage in both
  directions; it does not use limits, DIAG, or TMC UART.
*/
constexpr uint8_t STEP_PIN = 25;  // Vertical STEP
constexpr uint8_t DIR_PIN  = 26;  // Vertical DIR
constexpr uint8_t EN_PIN   = 27;  // Global driver enable, active-low

void setup() {
  pinMode(STEP_PIN, OUTPUT);
  pinMode(DIR_PIN, OUTPUT);
  pinMode(EN_PIN, OUTPUT);

  digitalWrite(EN_PIN, LOW);    // Enable all drivers (TMC2209 EN is active-low)
}

void moveSteps(long steps, bool direction, int delayMicros) {

  digitalWrite(DIR_PIN, direction);

  delayMicroseconds(10);

  for (long i = 0; i < steps; i++) {

    digitalWrite(STEP_PIN, HIGH);
    delayMicroseconds(delayMicros);

    digitalWrite(STEP_PIN, LOW);
    delayMicroseconds(delayMicros);
  }
}

void loop() {

  moveSteps(1600, HIGH, 400);   // Forward

  delay(1000);

  moveSteps(1600, LOW, 400);    // Reverse

  delay(1000);
}
