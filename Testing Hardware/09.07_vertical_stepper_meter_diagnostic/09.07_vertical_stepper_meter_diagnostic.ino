/*
  Meter-friendly vertical stepper diagnostic (no Wi-Fi, no libraries).

  IMPORTANT:
  - Test one TMC2209 and one disconnected-from-mechanics motor first.
  - Turn 24 V power OFF before changing any motor or driver wiring.
  - Open Serial Monitor at 115200 to see which test phase is active.
*/

constexpr uint8_t STEP_PIN = 25;
constexpr uint8_t DIR_PIN = 26;
constexpr uint8_t EN_PIN = 27;       // TMC2209 enable is active-low.
constexpr unsigned STEP_RATE_HZ = 100;
constexpr unsigned STEPS_PER_TEST = 800;
constexpr unsigned long METER_WINDOW_MS = 5000;

void setEnabled(bool enabled) {
  digitalWrite(EN_PIN, enabled ? LOW : HIGH);
}

void moveSlowly(bool direction) {
  digitalWrite(DIR_PIN, direction ? HIGH : LOW);
  delayMicroseconds(10);

  const unsigned long periodUs = 1000000UL / STEP_RATE_HZ;
  for (unsigned i = 0; i < STEPS_PER_TEST; ++i) {
    digitalWrite(STEP_PIN, HIGH);
    delayMicroseconds(5);
    digitalWrite(STEP_PIN, LOW);
    delayMicroseconds(periodUs - 5);
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(STEP_PIN, OUTPUT);
  pinMode(DIR_PIN, OUTPUT);
  pinMode(EN_PIN, OUTPUT);
  digitalWrite(STEP_PIN, LOW);
  digitalWrite(DIR_PIN, LOW);
  setEnabled(false);

  Serial.println("Vertical stepper meter diagnostic starting.");
  Serial.println("Driver DISABLED for 5 seconds: EN should measure about 3.3 V.");
  delay(METER_WINDOW_MS);

  Serial.println("Driver still DISABLED; STEP HIGH for 5 seconds.");
  Serial.println("STEP at the driver should measure about 3.3 V.");
  digitalWrite(STEP_PIN, HIGH);
  delay(METER_WINDOW_MS);
  digitalWrite(STEP_PIN, LOW);
}

void loop() {
  Serial.println("Driver ENABLED, no steps, for 5 seconds.");
  Serial.println("EN should measure about 0 V; motor shaft should become harder to turn.");
  setEnabled(true);
  digitalWrite(STEP_PIN, LOW);
  delay(METER_WINDOW_MS);

  Serial.println("DIR HIGH for 5 seconds: DIR should measure about 3.3 V.");
  digitalWrite(DIR_PIN, HIGH);
  delay(METER_WINDOW_MS);

  Serial.println("Sending 800 steps at 100 steps/second with DIR HIGH.");
  moveSlowly(HIGH);
  Serial.println("Motion stopped for 3 seconds; driver remains enabled.");
  delay(3000);

  Serial.println("DIR LOW for 5 seconds: DIR should measure about 0 V.");
  digitalWrite(DIR_PIN, LOW);
  delay(METER_WINDOW_MS);

  Serial.println("Sending 800 steps at 100 steps/second with DIR LOW.");
  moveSlowly(LOW);
  Serial.println("Motion stopped for 3 seconds; driver remains enabled, then test repeats.");
  delay(3000);
}
