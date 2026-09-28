/*
 * Smart Cruise E-Bike - PID cruise control
 * ---------------------------------------------------------------
 * ESP32 + X9C103S digital potentiometer + 5V relay + speed signal
 *
 * Idea:
 *   Cruise OFF : relay NC  -> manual throttle goes to the controller.
 *   Cruise ON  : relay NO  -> X9C103S wiper goes to the controller,
 *                             the ESP32 moves the wiper with a PID loop.
 *
 * Safety:
 *   - Relay is NC by default, so if the ESP32 resets or loses power,
 *     the rider gets the manual throttle back.
 *   - Brake, button press, lost speed signal, or very low speed
 *     all end cruise immediately.
 *
 * READ FIRST: see the CONFIG section and firmware/README.md.
 * Pins, speed calibration, and PID gains must be checked on the bike.
 */

#include <Arduino.h>

// ================================================================
// CONFIG: PINS  (change to match your wiring)
// ================================================================
constexpr uint8_t PIN_SPEED  = 34;  // speed signal input (input-only pin)
constexpr uint8_t PIN_BUTTON = 27;  // cruise button to GND (internal pull-up)
constexpr uint8_t PIN_BRAKE  = 26;  // brake cut-off signal (internal pull-up)
constexpr uint8_t PIN_RELAY  = 25;  // relay module IN
constexpr uint8_t PIN_X9_INC = 18;  // X9C103S INC
constexpr uint8_t PIN_X9_UD  = 19;  // X9C103S U/D
constexpr uint8_t PIN_X9_CS  = 23;  // X9C103S CS

// ================================================================
// CONFIG: LOGIC LEVELS
// ================================================================
constexpr bool RELAY_ACTIVE_HIGH = true;  // report: HIGH switches relay to NO
constexpr bool BRAKE_ACTIVE_LOW  = true;  // brake pressed = pin reads LOW

// ================================================================
// CONFIG: SPEED MEASUREMENT
// ================================================================
// speed (km/h) = pulse frequency (Hz) * KMH_PER_HZ
// Calibrate: ride at a steady speed shown on the display, read "hz"
// from the Serial Monitor, then KMH_PER_HZ = display_kmh / hz.
// Cruise still holds a constant speed even if this is a bit off.
constexpr float WHEEL_CIRCUMFERENCE_M  = 2.07f; // 26" wheel, measure yours
constexpr float PULSES_PER_WHEEL_REV   = 1.0f;  // CALIBRATE this
constexpr float KMH_PER_HZ = WHEEL_CIRCUMFERENCE_M * 3.6f / PULSES_PER_WHEEL_REV;

constexpr uint32_t MIN_PULSE_GAP_US   = 1000;     // ignore pulses closer than this (noise)
constexpr uint32_t SIGNAL_TIMEOUT_US  = 1500000;  // no pulse for 1.5 s = no speed
constexpr uint32_t SAMPLE_MS          = 200;      // control loop period
constexpr float    SPEED_FILTER_ALPHA = 0.5f;     // 0..1, higher = faster response

// ================================================================
// CONFIG: CRUISE LIMITS
// ================================================================
constexpr float MIN_CRUISE_KMH  = 8.0f;   // cannot engage below this
constexpr float MAX_CRUISE_KMH  = 30.0f;  // cannot engage above this
constexpr float EXIT_BELOW_KMH  = 4.0f;   // cruise ends if speed drops below this
constexpr float LOST_SPEED_KMH  = 8.0f;   // speed this far under set-point ...
constexpr uint32_t LOST_SPEED_MS = 4000;  // ... for this long ends cruise

// ================================================================
// CONFIG: X9C103S WIPER RANGE (0..99)
// ================================================================
// VH = 5V, VL = GND, so each step is about 0.05 V.
// Set the range to the throttle voltage window of YOUR controller
// (many controllers: about 1.1 V idle to 4.2 V full = steps 22 to 84).
// Measure the real throttle voltage with a multimeter first.
constexpr int WIPER_MIN = 24;
constexpr int WIPER_MAX = 80;
// Starting guess for the wiper when cruise engages:
// wiper = WIPER_MIN + speed / FF_MAX_SPEED_KMH * (WIPER_MAX - WIPER_MIN)
constexpr float FF_MAX_SPEED_KMH = 30.0f;
constexpr int   MAX_STEP_PER_LOOP = 3;    // limits how fast the throttle can change

// ================================================================
// CONFIG: PID GAINS (units: wiper steps per km/h)
// Start low. Tune on the road. See firmware/README.md.
// ================================================================
constexpr float KP = 1.0f;
constexpr float KI = 0.5f;
constexpr float KD = 0.2f;

constexpr bool ENABLE_LOG = true;   // CSV over Serial at 115200

// ================================================================
// SPEED INPUT (interrupt)
// ================================================================
volatile uint32_t isrCount   = 0;
volatile uint32_t lastPulseUs = 0;

void IRAM_ATTR onSpeedPulse() {
  uint32_t now = micros();
  if (now - lastPulseUs > MIN_PULSE_GAP_US) {
    isrCount++;
    lastPulseUs = now;
  }
}

// ================================================================
// X9C103S DRIVER
// The wiper moves on the falling edge of INC.
// INC is left LOW before CS goes HIGH, so the position is NOT saved
// to the chip memory (this protects it from wear).
// ================================================================
int wiperPos = 0;  // our own record of the wiper position (0..99)

void x9Init() {
  digitalWrite(PIN_X9_CS, HIGH);
  digitalWrite(PIN_X9_INC, HIGH);
  digitalWrite(PIN_X9_UD, HIGH);
  pinMode(PIN_X9_CS, OUTPUT);
  pinMode(PIN_X9_INC, OUTPUT);
  pinMode(PIN_X9_UD, OUTPUT);
}

void x9Step(bool up, int n) {
  if (n <= 0) return;
  digitalWrite(PIN_X9_UD, up ? HIGH : LOW);
  digitalWrite(PIN_X9_INC, HIGH);
  delayMicroseconds(5);
  digitalWrite(PIN_X9_CS, LOW);
  delayMicroseconds(5);
  for (int i = 0; i < n; i++) {
    digitalWrite(PIN_X9_INC, HIGH);
    delayMicroseconds(3);
    digitalWrite(PIN_X9_INC, LOW);   // falling edge = one step
    delayMicroseconds(3);
  }
  digitalWrite(PIN_X9_CS, HIGH);     // INC is LOW here: no save
  delayMicroseconds(5);
  digitalWrite(PIN_X9_INC, HIGH);
}

// Go to the bottom, so the real position is known (0).
void x9Home() {
  x9Step(false, 105);
  wiperPos = 0;
}

void x9MoveTo(int target) {
  target = constrain(target, 0, 99);
  int diff = target - wiperPos;
  if (diff > 0) x9Step(true, diff);
  else if (diff < 0) x9Step(false, -diff);
  wiperPos = target;
}

// ================================================================
// RELAY
// ================================================================
void setRelayCruise(bool cruiseOn) {
  digitalWrite(PIN_RELAY, (cruiseOn == RELAY_ACTIVE_HIGH) ? HIGH : LOW);
}

// ================================================================
// STATE
// ================================================================
enum Mode { MANUAL, CRUISE };
Mode mode = MANUAL;

float speedKmh   = 0.0f;   // filtered speed
float speedHz    = 0.0f;   // raw frequency (for calibration)
bool  speedValid = false;

float setpoint   = 0.0f;
float integ      = 0.0f;
float prevSpeed  = 0.0f;
uint32_t lostSince = 0;

uint32_t prevCount = 0;
uint32_t lastSampleMs = 0;

// button
bool btnLast = HIGH;
uint32_t btnChangeMs = 0;

bool brakePressed() {
  bool level = digitalRead(PIN_BRAKE);
  return BRAKE_ACTIVE_LOW ? (level == LOW) : (level == HIGH);
}

// ================================================================
// CRUISE ON / OFF
// ================================================================
void exitCruise(const char* reason) {
  setRelayCruise(false);          // 1) manual throttle back FIRST
  mode = MANUAL;
  x9Home();                       // 2) park the wiper at 0
  if (ENABLE_LOG) {
    Serial.print("# CRUISE OFF: ");
    Serial.println(reason);
  }
}

void engageCruise() {
  if (!speedValid)               { if (ENABLE_LOG) Serial.println("# no engage: no speed signal"); return; }
  if (speedKmh < MIN_CRUISE_KMH) { if (ENABLE_LOG) Serial.println("# no engage: too slow");       return; }
  if (speedKmh > MAX_CRUISE_KMH) { if (ENABLE_LOG) Serial.println("# no engage: too fast");       return; }
  if (brakePressed())            { if (ENABLE_LOG) Serial.println("# no engage: brake pressed");  return; }

  setpoint = speedKmh;

  // Put the wiper near the throttle level the rider is using, BEFORE
  // the relay switches, so the bike does not jump.
  float ff = WIPER_MIN + (speedKmh / FF_MAX_SPEED_KMH) * (WIPER_MAX - WIPER_MIN);
  ff = constrain(ff, (float)WIPER_MIN, (float)WIPER_MAX);
  x9MoveTo((int)lroundf(ff));

  integ     = ff;
  prevSpeed = speedKmh;
  lostSince = 0;

  setRelayCruise(true);
  mode = CRUISE;
  if (ENABLE_LOG) {
    Serial.print("# CRUISE ON, set-point km/h = ");
    Serial.println(setpoint, 1);
  }
}

// ================================================================
// SPEED SAMPLE (every SAMPLE_MS)
// ================================================================
void updateSpeed(float dtSec) {
  uint32_t count = isrCount;
  uint32_t delta = count - prevCount;
  prevCount = count;

  speedHz = delta / dtSec;
  float raw = speedHz * KMH_PER_HZ;

  uint32_t sinceLast = micros() - lastPulseUs;
  if (sinceLast > SIGNAL_TIMEOUT_US) {
    speedValid = false;
    raw = 0.0f;
    speedKmh = 0.0f;
  } else {
    speedValid = true;
    speedKmh = SPEED_FILTER_ALPHA * raw + (1.0f - SPEED_FILTER_ALPHA) * speedKmh;
  }
}

// ================================================================
// PID (runs every SAMPLE_MS while cruise is ON)
// ================================================================
void runPid(float dtSec) {
  float e = setpoint - speedKmh;

  // Integral with clamp (anti-windup)
  integ += KI * e * dtSec;
  integ = constrain(integ, (float)WIPER_MIN, (float)WIPER_MAX);

  // Derivative on the measurement (no kick when set-point changes)
  float dSpeed = (speedKmh - prevSpeed) / dtSec;
  prevSpeed = speedKmh;

  float out = integ + KP * e - KD * dSpeed;
  out = constrain(out, (float)WIPER_MIN, (float)WIPER_MAX);

  int target = (int)lroundf(out);
  int delta = constrain(target - wiperPos, -MAX_STEP_PER_LOOP, MAX_STEP_PER_LOOP);
  if (delta != 0) x9MoveTo(wiperPos + delta);
}

// ================================================================
// SETUP
// ================================================================
void setup() {
  Serial.begin(115200);

  // Relay: set the level BEFORE making the pin an output (no glitch).
  digitalWrite(PIN_RELAY, RELAY_ACTIVE_HIGH ? LOW : HIGH);  // NC = manual
  pinMode(PIN_RELAY, OUTPUT);

  pinMode(PIN_BUTTON, INPUT_PULLUP);
  pinMode(PIN_BRAKE, INPUT_PULLUP);
  pinMode(PIN_SPEED, INPUT);

  x9Init();
  x9Home();                        // relay is NC now, so this is safe

  attachInterrupt(digitalPinToInterrupt(PIN_SPEED), onSpeedPulse, RISING);

  lastSampleMs = millis();
  if (ENABLE_LOG) {
    Serial.println("# Smart Cruise E-Bike ready");
    Serial.println("ms,mode,hz,speed_kmh,setpoint_kmh,wiper");
  }
}

// ================================================================
// LOOP
// ================================================================
void loop() {
  uint32_t nowMs = millis();

  // ---- Brake: checked all the time, fastest response ----
  if (mode == CRUISE && brakePressed()) {
    exitCruise("brake");
  }

  // ---- Button (debounced, acts on press) ----
  bool btn = digitalRead(PIN_BUTTON);
  if (btn != btnLast && (nowMs - btnChangeMs) > 40) {
    btnChangeMs = nowMs;
    btnLast = btn;
    if (btn == LOW) {                       // pressed
      if (mode == MANUAL) engageCruise();
      else                exitCruise("button");
    }
  }

  // ---- Periodic sample ----
  if (nowMs - lastSampleMs >= SAMPLE_MS) {
    float dtSec = (nowMs - lastSampleMs) / 1000.0f;
    lastSampleMs = nowMs;

    updateSpeed(dtSec);

    if (mode == CRUISE) {
      if (!speedValid) {
        exitCruise("no speed signal");
      } else if (speedKmh < EXIT_BELOW_KMH) {
        exitCruise("speed too low");
      } else {
        // Cannot hold the speed (steep hill, weak battery, etc.)
        if (speedKmh < setpoint - LOST_SPEED_KMH) {
          if (lostSince == 0) lostSince = nowMs;
          if (nowMs - lostSince > LOST_SPEED_MS) exitCruise("cannot hold speed");
        } else {
          lostSince = 0;
        }
      }
      if (mode == CRUISE) runPid(dtSec);
    }

    if (ENABLE_LOG) {
      Serial.print(nowMs);           Serial.print(',');
      Serial.print(mode == CRUISE ? "CRUISE" : "MANUAL"); Serial.print(',');
      Serial.print(speedHz, 1);      Serial.print(',');
      Serial.print(speedKmh, 1);     Serial.print(',');
      Serial.print(mode == CRUISE ? setpoint : 0.0f, 1); Serial.print(',');
      Serial.println(wiperPos);
    }
  }
}
