/*
  vertical_stepper_serial_test.ino

  Offline manual vertical-stepper test for the ESP32.  There is no Wi-Fi,
  web server, WebSocket, or browser required.  Use the USB Serial Monitor at
  115200 baud (set line ending to "Newline" or "Both NL & CR").

  Controls:
    U  move up continuously
    D  move down continuously
    S or Space  stop (smooth deceleration, then disable drivers)
    M  enter a new maximum speed in steps/second on the next line
    H or ?  show this help

  GPIO 25 (STEP) and GPIO 26 (DIR) are shared by both vertical drivers, so
  both vertical motors receive the same command. GPIO 27 is the active-low
  TMC2209 enable. This test has no limit switches, DIAG, or TMC UART safety
  features. Keep clear of the machine and be ready to remove motor power.

  Required library: AccelStepper.
*/

#include <AccelStepper.h>

constexpr uint8_t VERTICAL_STEP_PIN = 25;
constexpr uint8_t VERTICAL_DIR_PIN = 26;
constexpr uint8_t DRIVER_ENABLE_PIN = 27;  // TMC2209 EN is active-low.
constexpr bool VERTICAL_DIR_UP_LEVEL = HIGH; // Change to LOW if U moves down.
constexpr float ACCELERATION_STEPS_PER_SECOND2 = 800.0f;
constexpr float MIN_SPEED_STEPS_PER_SECOND = 10.0f;
constexpr float MAX_ALLOWED_SPEED_STEPS_PER_SECOND = 5000.0f;
constexpr long CONTINUOUS_TARGET_STEPS = 100000000L;

AccelStepper verticalStepper(AccelStepper::DRIVER, VERTICAL_STEP_PIN, VERTICAL_DIR_PIN);

float maxSpeed = 400.0f;
int8_t commandedDirection = 0; // +1 up, -1 down, 0 stopped
bool enteringSpeed = false;
String speedInput;

void driversEnabled(bool enabled) {
  digitalWrite(DRIVER_ENABLE_PIN, enabled ? LOW : HIGH);
}

void printStatus(const char *prefix = "") {
  Serial.printf("%s direction=%d, maxSpeed=%.1f steps/s\n", prefix,
                commandedDirection, maxSpeed);
}

void printHelp() {
  Serial.println("Commands: U=up, D=down, S or Space=stop, M=set speed, H or ?=help");
  Serial.println("After M, send a speed from 10 to 5000 steps/second, then press Enter.");
}

void stopMotion(const char *reason) {
  commandedDirection = 0;
  verticalStepper.stop();  // Decelerates smoothly from the current speed.
  Serial.printf("Stopping: %s\n", reason);
}

void startMotion(int8_t direction) {
  commandedDirection = direction;
  driversEnabled(true);
  verticalStepper.moveTo(direction > 0 ? CONTINUOUS_TARGET_STEPS
                                       : -CONTINUOUS_TARGET_STEPS);
  printStatus(direction > 0 ? "Moving up." : "Moving down.");
}

void finishSpeedEntry() {
  float requested = speedInput.toFloat();
  if (requested >= MIN_SPEED_STEPS_PER_SECOND && requested <= MAX_ALLOWED_SPEED_STEPS_PER_SECOND) {
    maxSpeed = requested;
    verticalStepper.setMaxSpeed(maxSpeed);
    printStatus("Speed updated.");
  } else {
    Serial.printf("Invalid speed. Use %.0f to %.0f steps/second.\n",
                  MIN_SPEED_STEPS_PER_SECOND, MAX_ALLOWED_SPEED_STEPS_PER_SECOND);
  }
  speedInput = "";
  enteringSpeed = false;
}

void handleSerial() {
  while (Serial.available()) {
    char key = static_cast<char>(Serial.read());

    if (enteringSpeed) {
      if (key == '\n' || key == '\r') {
        if (speedInput.length() > 0) finishSpeedEntry();
      } else if ((key >= '0' && key <= '9') || key == '.') {
        speedInput += key;
      } else if (key == '-' && speedInput.length() == 0) {
        speedInput += key;
      }
      continue;
    }

    if (key == '\r' || key == '\n') continue;
    switch (tolower(key)) {
      case 'u': startMotion(1); break;
      case 'd': startMotion(-1); break;
      case 's':
      case ' ': stopMotion("serial stop command"); break;
      case 'm':
        enteringSpeed = true;
        speedInput = "";
        Serial.printf("Enter maximum speed (%.0f to %.0f steps/second), then press Enter: ",
                      MIN_SPEED_STEPS_PER_SECOND, MAX_ALLOWED_SPEED_STEPS_PER_SECOND);
        break;
      case 'h':
      case '?': printHelp(); break;
      default: Serial.printf("Unknown command '%c'. Type H for help.\n", key); break;
    }
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(DRIVER_ENABLE_PIN, OUTPUT);
  driversEnabled(false);  // Safe reset/startup state.

  verticalStepper.setPinsInverted(!VERTICAL_DIR_UP_LEVEL, false, false);
  verticalStepper.setMaxSpeed(maxSpeed);
  verticalStepper.setAcceleration(ACCELERATION_STEPS_PER_SECOND2);

  Serial.println("Offline vertical-stepper serial test ready.");
  printHelp();
}

void loop() {
  handleSerial();
  verticalStepper.run();

  // Disable only after AccelStepper has fully decelerated to a stop.
  if (commandedDirection == 0 && verticalStepper.distanceToGo() == 0 &&
      verticalStepper.speed() == 0) {
    driversEnabled(false);
  }
}
