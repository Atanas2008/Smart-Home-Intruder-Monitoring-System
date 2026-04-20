/*
 * Intruder Detection and Tracking System
 * Hardware: Arduino Uno, 3x HC-SR04 ultrasonic sensors, 1x Servo motor
 *
 * Sensor wiring:
 *   LEFT   sensor: TRIG=2,  ECHO=3
 *   CENTER sensor: TRIG=4,  ECHO=5
 *   RIGHT  sensor: TRIG=6,  ECHO=7
 *   Servo signal pin: 9
 *
 * Serial output (9600 baud) – parsed by the Python backend:
 *   STATE:<state>|POS:<position>|LEFT:<cm>|CENTER:<cm>|RIGHT:<cm>
 *
 * States:  IDLE | DETECTED | TRACKING | ALERT
 * Positions: NONE | LEFT | CENTER | RIGHT
 */

#include <Servo.h>

// ── Pin constants ─────────────────────────────────────────────
const uint8_t TRIG_LEFT   = 2;
const uint8_t ECHO_LEFT   = 3;
const uint8_t TRIG_CENTER = 4;
const uint8_t ECHO_CENTER = 5;
const uint8_t TRIG_RIGHT  = 6;
const uint8_t ECHO_RIGHT  = 7;
const uint8_t SERVO_PIN   = 9;

// ── System constants ──────────────────────────────────────────
const float   DETECTION_THRESHOLD_CM = 100.0; // intruder distance threshold
const unsigned long ALERT_TIMEOUT_MS = 5000;  // ms before state → ALERT
const unsigned long REPORT_INTERVAL_MS = 200; // serial report cadence
const unsigned long SENSOR_DELAY_US  = 100;   // guard delay between sensors

// Servo angles for each position
const int SERVO_LEFT   = 30;
const int SERVO_CENTER = 90;
const int SERVO_RIGHT  = 150;
const int SERVO_IDLE   = 90;

// Moving-average filter depth per sensor
const int FILTER_DEPTH = 5;

// Maximum credible distance from HC-SR04 (cm)
const float MAX_DISTANCE_CM = 400.0;

// ── State machine ─────────────────────────────────────────────
enum SystemState { IDLE, DETECTED, TRACKING, ALERT };
const char* STATE_NAMES[] = { "IDLE", "DETECTED", "TRACKING", "ALERT" };

enum Position { POS_NONE, POS_LEFT, POS_CENTER, POS_RIGHT };
const char* POSITION_NAMES[] = { "NONE", "LEFT", "CENTER", "RIGHT" };

// ── Moving-average buffers ────────────────────────────────────
float bufLeft[FILTER_DEPTH];
float bufCenter[FILTER_DEPTH];
float bufRight[FILTER_DEPTH];
int   filterIndex = 0;
bool  filterFull  = false;

// ── Runtime state ─────────────────────────────────────────────
Servo trackingServo;
SystemState currentState    = IDLE;
Position    currentPosition = POS_NONE;
int         currentServoAngle = SERVO_IDLE;

unsigned long detectionStartMs  = 0;
unsigned long lastReportMs      = 0;

// ═══════════════════════════════════════════════════════════════
// Ultrasonic helpers
// ═══════════════════════════════════════════════════════════════

// Trigger a single HC-SR04 measurement and return distance in cm.
// Returns MAX_DISTANCE_CM when no echo is received within timeout.
float measureDistance(uint8_t trigPin, uint8_t echoPin) {
    // Ensure trig is low
    digitalWrite(trigPin, LOW);
    delayMicroseconds(2);

    // 10 µs HIGH pulse triggers measurement
    digitalWrite(trigPin, HIGH);
    delayMicroseconds(10);
    digitalWrite(trigPin, LOW);

    // Read echo pulse width (timeout = 30 ms ≈ ~515 cm round-trip)
    unsigned long duration = pulseIn(echoPin, HIGH, 30000UL);
    if (duration == 0) {
        return MAX_DISTANCE_CM;
    }
    float distance = (duration / 2.0) * 0.0343; // speed of sound cm/µs
    return constrain(distance, 0.0, MAX_DISTANCE_CM);
}

// ─── Moving-average filter ────────────────────────────────────

void initFilter() {
    for (int i = 0; i < FILTER_DEPTH; i++) {
        bufLeft[i]   = MAX_DISTANCE_CM;
        bufCenter[i] = MAX_DISTANCE_CM;
        bufRight[i]  = MAX_DISTANCE_CM;
    }
}

float average(float* buf) {
    int count = filterFull ? FILTER_DEPTH : (filterIndex == 0 ? 1 : filterIndex);
    float sum = 0;
    for (int i = 0; i < count; i++) sum += buf[i];
    return sum / count;
}

// Read all three sensors sequentially and push into filter buffers.
// Returns filtered distances via output parameters.
void readAllSensors(float &distLeft, float &distCenter, float &distRight) {
    bufLeft[filterIndex]   = measureDistance(TRIG_LEFT,   ECHO_LEFT);
    delayMicroseconds(SENSOR_DELAY_US);

    bufCenter[filterIndex] = measureDistance(TRIG_CENTER, ECHO_CENTER);
    delayMicroseconds(SENSOR_DELAY_US);

    bufRight[filterIndex]  = measureDistance(TRIG_RIGHT,  ECHO_RIGHT);

    filterIndex++;
    if (filterIndex >= FILTER_DEPTH) {
        filterIndex = 0;
        filterFull  = true;
    }

    distLeft   = average(bufLeft);
    distCenter = average(bufCenter);
    distRight  = average(bufRight);
}

// ═══════════════════════════════════════════════════════════════
// Position detection
// ═══════════════════════════════════════════════════════════════

Position detectPosition(float dLeft, float dCenter, float dRight) {
    bool iLeft   = (dLeft   < DETECTION_THRESHOLD_CM);
    bool iCenter = (dCenter < DETECTION_THRESHOLD_CM);
    bool iRight  = (dRight  < DETECTION_THRESHOLD_CM);

    if (!iLeft && !iCenter && !iRight) return POS_NONE;

    // Choose the closest active sensor as the primary position
    float minDist = MAX_DISTANCE_CM;
    Position pos  = POS_NONE;

    if (iLeft   && dLeft   < minDist) { minDist = dLeft;   pos = POS_LEFT;   }
    if (iCenter && dCenter < minDist) { minDist = dCenter; pos = POS_CENTER; }
    if (iRight  && dRight  < minDist) { minDist = dRight;  pos = POS_RIGHT;  }

    return pos;
}

// ═══════════════════════════════════════════════════════════════
// Servo control
// ═══════════════════════════════════════════════════════════════

void moveServeTo(int angle) {
    if (angle != currentServoAngle) {
        trackingServo.write(angle);
        currentServoAngle = angle;
    }
}

void updateServo(Position pos) {
    switch (pos) {
        case POS_LEFT:   moveServeTo(SERVO_LEFT);   break;
        case POS_CENTER: moveServeTo(SERVO_CENTER); break;
        case POS_RIGHT:  moveServeTo(SERVO_RIGHT);  break;
        default:         moveServeTo(SERVO_IDLE);   break;
    }
}

// ═══════════════════════════════════════════════════════════════
// State machine
// ═══════════════════════════════════════════════════════════════

void updateStateMachine(Position pos) {
    switch (currentState) {
        case IDLE:
            if (pos != POS_NONE) {
                currentState      = DETECTED;
                detectionStartMs  = millis();
            }
            break;

        case DETECTED:
            if (pos == POS_NONE) {
                currentState = IDLE;
            } else if (pos != currentPosition) {
                currentState = TRACKING;
            } else if (millis() - detectionStartMs > ALERT_TIMEOUT_MS) {
                currentState = ALERT;
            }
            break;

        case TRACKING:
            if (pos == POS_NONE) {
                currentState = IDLE;
            } else if (millis() - detectionStartMs > ALERT_TIMEOUT_MS) {
                currentState = ALERT;
            }
            break;

        case ALERT:
            if (pos == POS_NONE) {
                currentState     = IDLE;
                detectionStartMs = 0;
            }
            break;
    }
}

// ═══════════════════════════════════════════════════════════════
// Serial reporting
// ═══════════════════════════════════════════════════════════════

void reportStatus(float dLeft, float dCenter, float dRight) {
    unsigned long now = millis();
    if (now - lastReportMs < REPORT_INTERVAL_MS) return;
    lastReportMs = now;

    // Machine-readable line consumed by the Python backend
    Serial.print("STATE:");
    Serial.print(STATE_NAMES[currentState]);
    Serial.print("|POS:");
    Serial.print(POSITION_NAMES[currentPosition]);
    Serial.print("|LEFT:");
    Serial.print(dLeft, 1);
    Serial.print("|CENTER:");
    Serial.print(dCenter, 1);
    Serial.print("|RIGHT:");
    Serial.println(dRight, 1);
}

// ═══════════════════════════════════════════════════════════════
// Arduino entry points
// ═══════════════════════════════════════════════════════════════

void setup() {
    Serial.begin(9600);

    // Sensor pins
    pinMode(TRIG_LEFT,   OUTPUT);
    pinMode(ECHO_LEFT,   INPUT);
    pinMode(TRIG_CENTER, OUTPUT);
    pinMode(ECHO_CENTER, INPUT);
    pinMode(TRIG_RIGHT,  OUTPUT);
    pinMode(ECHO_RIGHT,  INPUT);

    // Servo
    trackingServo.attach(SERVO_PIN);
    trackingServo.write(SERVO_IDLE);
    currentServoAngle = SERVO_IDLE;

    // Filter buffers
    initFilter();

    Serial.println("STATE:IDLE|POS:NONE|LEFT:0.0|CENTER:0.0|RIGHT:0.0");
}

void loop() {
    float dLeft, dCenter, dRight;
    readAllSensors(dLeft, dCenter, dRight);

    Position pos = detectPosition(dLeft, dCenter, dRight);
    updateStateMachine(pos);
    currentPosition = pos;

    updateServo(pos);
    reportStatus(dLeft, dCenter, dRight);
}
