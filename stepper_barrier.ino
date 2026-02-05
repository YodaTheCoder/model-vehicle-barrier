#include <AccelStepper.h>
#include <Adafruit_NeoPixel.h>

/* =========================
   PIN CONFIG
   ========================= */
const int TRIG_PIN = 9;
const int ECHO_PIN = 10;
const int DIR_SWITCH_PIN = 3;
const int JOG_BUTTON_PIN = 2;

// ULN2003 pins (IN1, IN3, IN2, IN4)
AccelStepper stepper(
  AccelStepper::HALF4WIRE,
  5, 7, 6, 8
);

// NeoPixel
const int NEOPIXEL_PIN = 4;
const int NUM_LEDS = 2;
Adafruit_NeoPixel pixels(NUM_LEDS, NEOPIXEL_PIN, NEO_GRB + NEO_KHZ800);

/* =========================
   BARRIER CONFIG
   ========================= */
const long BARRIER_RANGE = 1000;            // travel in steps from DOWN to UP
const int  CLOSE_DISTANCE_CM = 20;
const unsigned long LOWER_DELAY_MS = 3000;

/* =========================
   JOG CONFIG
   ========================= */
const int JOG_STEP_SIZE = 5;               // steps per press (fine adjust)
const unsigned long JOG_DEBOUNCE_MS = 50;  // simple debounce

/* =========================
   STEPPER TUNING
   ========================= */
const float MAX_SPEED    = 1500.0;  // steps/sec
const float ACCELERATION = 2000.0;  // steps/sec^2

/* =========================
   ULTRASONIC TIMING
   ========================= */
const unsigned long US_INTERVAL_MS = 50;
const unsigned long US_TIMEOUT_US  = 25000;

/* =========================
   ULTRASONIC STATE
   ========================= */
enum UltrasonicState { US_IDLE, US_WAIT_RISE, US_WAIT_FALL };
UltrasonicState usState = US_IDLE;

unsigned long usLastPingMs   = 0;
unsigned long usTriggerAtUs  = 0;
unsigned long usEchoRiseAtUs = 0;
float lastDistanceCm = -1;

/* =========================
   BARRIER STATE
   ========================= */
bool vehiclePresent = false;
unsigned long vehicleClearedAt = 0;

/* =========================
   CALIBRATED DOWN POSITION
   ========================= */
long downPos = 0;  // this is what the jog button calibrates

void setup() {
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  pinMode(DIR_SWITCH_PIN, INPUT_PULLUP);
  pinMode(JOG_BUTTON_PIN, INPUT_PULLUP);

  digitalWrite(TRIG_PIN, LOW);

  stepper.setMaxSpeed(MAX_SPEED);
  stepper.setAcceleration(ACCELERATION);
  stepper.setCurrentPosition(0);

  pixels.begin();
  pixels.clear();
  pixels.show();

  Serial.begin(9600);
  Serial.println("Barrier system ready (jog calibrates DOWN)");
}

void loop() {
  unsigned long nowMs = millis();

  // Always run stepper
  stepper.run();

  // Direction (side of road)
  bool reverse = (digitalRead(DIR_SWITCH_PIN) == LOW);

  long upPos = downPos + (reverse ? -BARRIER_RANGE : BARRIER_RANGE);

  /* =========================
     JOG BUTTON (MULTI-PRESS, DOWN ONLY)
     ========================= */
  static bool lastJogState = HIGH;
  static unsigned long lastJogEdgeAt = 0;

  bool jogState = digitalRead(JOG_BUTTON_PIN);

  // "Down / red light on" in your system means: not fully up.
  // But we also only want jog when we're stationary at the DOWN target (not mid-move).
  bool atDownTarget =
    (stepper.distanceToGo() == 0) &&
    (stepper.currentPosition() == downPos) &&
    (stepper.targetPosition() == downPos);

  if (atDownTarget &&
      lastJogState == HIGH && jogState == LOW &&
      (nowMs - lastJogEdgeAt) >= JOG_DEBOUNCE_MS) {

    lastJogEdgeAt = nowMs;

    long jogDelta = reverse ? -JOG_STEP_SIZE : JOG_STEP_SIZE;

    // Calibrate DOWN by nudging the down target itself
    downPos += jogDelta;

    // Command motor to the new DOWN target
    stepper.moveTo(downPos);

    Serial.print("Jog: new downPos = ");
    Serial.println(downPos);
  }

  lastJogState = jogState;

  /* =========================
     UPDATE ULTRASONIC (NON-BLOCKING)
     ========================= */
  updateUltrasonic();

  /* =========================
     AUTOMATIC BARRIER LOGIC
     ========================= */
  if (lastDistanceCm >= 0) {

    if (lastDistanceCm <= CLOSE_DISTANCE_CM) {
      vehiclePresent = true;
      vehicleClearedAt = 0;

      if (stepper.targetPosition() != upPos) {
        stepper.moveTo(upPos);
      }
    } else {
      if (vehiclePresent) {
        vehiclePresent = false;
        vehicleClearedAt = nowMs;
      }

      if (vehicleClearedAt > 0 &&
          nowMs - vehicleClearedAt >= LOWER_DELAY_MS &&
          stepper.targetPosition() != downPos) {

        stepper.moveTo(downPos);
      }
    }
  }

  updateTrafficLights(upPos);
}

/* =========================
   FIXED NON-BLOCKING ULTRASONIC
   ========================= */
void updateUltrasonic() {
  unsigned long nowMs = millis();
  unsigned long nowUs = micros();

  switch (usState) {
    case US_IDLE:
      if (nowMs - usLastPingMs >= US_INTERVAL_MS) {
        digitalWrite(TRIG_PIN, LOW);
        delayMicroseconds(2);
        digitalWrite(TRIG_PIN, HIGH);
        delayMicroseconds(10);
        digitalWrite(TRIG_PIN, LOW);

        usLastPingMs  = nowMs;
        usTriggerAtUs = nowUs;
        usState = US_WAIT_RISE;
      }
      break;

    case US_WAIT_RISE:
      if (digitalRead(ECHO_PIN) == HIGH) {
        usEchoRiseAtUs = nowUs;
        usState = US_WAIT_FALL;
      } else if (nowUs - usTriggerAtUs > US_TIMEOUT_US) {
        usState = US_IDLE;
      }
      break;

    case US_WAIT_FALL:
      if (digitalRead(ECHO_PIN) == LOW) {
        unsigned long echoDurationUs = nowUs - usEchoRiseAtUs;
        lastDistanceCm = echoDurationUs * 0.0343f / 2.0f;
        usState = US_IDLE;
      } else if (nowUs - usEchoRiseAtUs > US_TIMEOUT_US) {
        usState = US_IDLE;
      }
      break;
  }
}

/* =========================
   TRAFFIC LIGHT LOGIC
   ========================= */
void updateTrafficLights(long barrierUpPos) {
  static int lastState = -1;

  bool barrierFullyUp =
    (stepper.currentPosition() == barrierUpPos) &&
    (stepper.distanceToGo() == 0);

  int state = barrierFullyUp ? 1 : 0;

  if (state != lastState) {
    pixels.clear();

    if (barrierFullyUp) {
      pixels.setPixelColor(0, pixels.Color(0, 255, 0)); // Green (LED 0)
    } else {
      pixels.setPixelColor(1, pixels.Color(255, 0, 0)); // Red (LED 1)
    }

    pixels.show();
    lastState = state;
  }
}
