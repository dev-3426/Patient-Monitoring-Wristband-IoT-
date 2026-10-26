#include <Wire.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

#define ENABLE_DATABASE
#include <FirebaseClient.h>
#include <MAX30105.h>
#include "spo2_algorithm.h"
#include <MPU6050_tockn.h>
#include <TinyGPSPlus.h>
#include <math.h>

// ==================================================
// Pin configuration
// ==================================================

// MAX30102
#define MAX_SDA 21
#define MAX_SCL 22

// MPU6050
#define MPU_SDA 26
#define MPU_SCL 27

// NEO-6M GPS
#define GPS_RX_PIN 16   // GPS TX -> ESP32 GPIO16
#define GPS_TX_PIN 17   // GPS RX -> ESP32 GPIO17
#define GPS_BAUD 9600

// ==================================================
// Sensor objects
// ==================================================

MAX30105 maxSensor;

TwoWire MPUWire = TwoWire(1);
MPU6050 mpu(MPUWire);

TinyGPSPlus gps;
HardwareSerial gpsSerial(2);


// ==================================================
// Wi-Fi and Firebase configuration
// ==================================================

// Replace these two values only in Arduino IDE.
#define WIFI_SSID "YOUR_WIFI_NAME"
#define WIFI_PASSWORD "YOUR_WIFI_PASSWORD"

#define DATABASE_URL \
"https://neuroband-502a6-default-rtdb.asia-southeast1.firebasedatabase.app"

NoAuth noAuth;
FirebaseApp firebaseApp;
WiFiClientSecure firebaseSSL;
AsyncClientClass firebaseClient(firebaseSSL);
RealtimeDatabase database;

bool firebaseUploadCycleActive = false;
int firebaseUploadStep = 0;

unsigned long lastFirebaseUploadTime = 0;
unsigned long lastFirebaseFieldTime = 0;

const unsigned long FIREBASE_UPLOAD_INTERVAL = 5000;
const unsigned long FIREBASE_FIELD_INTERVAL = 250;

// ==================================================
// MAX30102 variables
// ==================================================

const int MAX_BUFFER_LENGTH = 100;
const int NEW_SAMPLES = 25;

uint32_t irBuffer[MAX_BUFFER_LENGTH];
uint32_t redBuffer[MAX_BUFFER_LENGTH];

uint32_t currentIR = 0;
uint32_t currentRed = 0;

int32_t heartRate = 0;
int32_t spo2 = 0;

int8_t heartRateValid = 0;
int8_t spo2Valid = 0;

int samplesCollected = 0;

bool initialBufferReady = false;
bool fingerDetected = false;
bool maxSaturated = false;

// Stable health-value filtering
const int REQUIRED_STABLE_READINGS = 3;
const int HEART_RATE_MIN = 45;
const int HEART_RATE_MAX = 130;
const int SPO2_MIN = 90;
const int SPO2_MAX = 100;
const int HEART_RATE_MAX_JUMP = 20;
const int SPO2_MAX_JUMP = 3;

int stableHeartRate = 0;
int stableSpO2 = 0;
int previousAcceptedHeartRate = 0;
int previousAcceptedSpO2 = 0;
int heartRateCandidateSum = 0;
int spo2CandidateSum = 0;
int heartRateStableCount = 0;
int spo2StableCount = 0;

bool stableHeartRateAvailable = false;
bool stableSpO2Available = false;

// ==================================================
// MPU6050 variables
// ==================================================

float accX = 0.0;
float accY = 0.0;
float accZ = 0.0;

float gyroX = 0.0;
float gyroY = 0.0;
float gyroZ = 0.0;

float accelerationMagnitude = 0.0;

// ==================================================
// Fall detection variables
// ==================================================

bool possibleFreeFall = false;
bool fallDetected = false;

unsigned long freeFallStartTime = 0;
unsigned long fallAlertStartTime = 0;

const float FREE_FALL_THRESHOLD = 0.45;
const float IMPACT_THRESHOLD = 2.20;

const unsigned long IMPACT_TIME_LIMIT = 1500;
const unsigned long FALL_ALERT_DURATION = 5000;

// ==================================================
// GPS variables
// ==================================================

bool gpsFixAvailable = false;

double latitude = 0.0;
double longitude = 0.0;

uint32_t satelliteCount = 0;
double gpsHDOP = 0.0;

const unsigned long GPS_DATA_TIMEOUT = 5000;

// ==================================================
// Geofence settings
// ==================================================

// Temporary safe-zone centre for local testing
const double SAFE_ZONE_LATITUDE = 9.956748;
const double SAFE_ZONE_LONGITUDE = 78.151152;

// Radius in metres
const double SAFE_ZONE_RADIUS_METERS = 5.0;

double distanceFromSafeZone = 0.0;

bool geofenceAvailable = false;
bool outsideGeofence = false;


// ==================================================
// Indoor Wi-Fi positioning
// ==================================================

const char* OFFICE_SSID = "JioFiber-f3XJJ";
const char* OFFICE_BSSID = "AA:83:94:B7:46:DD";

const int OFFICE_MIN_RSSI = -80;

bool officeDetected = false;
int officeRSSI = -100;

// ==================================================
// Display timing
// ==================================================

unsigned long lastDisplayTime = 0;

const unsigned long DISPLAY_INTERVAL = 1000;

// ==================================================
// Function declarations
// ==================================================

bool setupMAX30102();
void setupMPU6050();
void setupGPS();
void connectWiFi();
void setupFirebase();
void setupIndoorWiFi();

void updateMAX30102();
void updateMPU6050();
void updateGPS();
void updateIndoorWiFi();
void updateFirebase();

void calculateHealthValues();
void updateStableHealthValues();
void resetStableHealthValues();
void shiftMAXBuffer();

void detectFall();
void updateGeofence();

void processFirebaseResult(AsyncResult &result);
void uploadNextFirebaseField();

void displayReadings();

// ==================================================
// MAX30102 setup
// ==================================================

bool setupMAX30102() {
  Wire.begin(MAX_SDA, MAX_SCL);

  if (!maxSensor.begin(Wire, I2C_SPEED_STANDARD)) {
    return false;
  }

  byte ledBrightness = 50;
  byte sampleAverage = 4;
  byte ledMode = 2;

  int sampleRate = 100;
  int pulseWidth = 411;
  int adcRange = 16384;

  maxSensor.setup(
    ledBrightness,
    sampleAverage,
    ledMode,
    sampleRate,
    pulseWidth,
    adcRange
  );

  maxSensor.setPulseAmplitudeRed(50);
  maxSensor.setPulseAmplitudeIR(50);
  maxSensor.setPulseAmplitudeGreen(0);

  maxSensor.clearFIFO();

  return true;
}

// ==================================================
// MPU6050 setup
// ==================================================

void setupMPU6050() {
  MPUWire.begin(MPU_SDA, MPU_SCL);

  mpu.begin();

  Serial.println("Keep MPU6050 still for calibration...");
  mpu.calcGyroOffsets(true);
}

// ==================================================
// GPS setup
// ==================================================

void setupGPS() {
  gpsSerial.begin(
    GPS_BAUD,
    SERIAL_8N1,
    GPS_RX_PIN,
    GPS_TX_PIN
  );

  Serial.println("NEO-6M GPS UART started.");
}


// ==================================================
// Wi-Fi connection
// ==================================================

void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  Serial.print("Connecting to Wi-Fi");

  while (WiFi.status() != WL_CONNECTED) {
    Serial.print(".");
    delay(500);
  }

  Serial.println();
  Serial.println("Wi-Fi connected");

  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());
}

// ==================================================
// Firebase setup
// ==================================================

void processFirebaseResult(AsyncResult &result) {
  if (!result.isResult()) {
    return;
  }

  if (result.isError()) {
    Serial.print("Firebase error: ");
    Serial.println(result.error().message());
  }
}

void setupFirebase() {
  firebaseSSL.setInsecure();

  initializeApp(
    firebaseClient,
    firebaseApp,
    getAuth(noAuth),
    processFirebaseResult,
    "firebaseAuth"
  );

  firebaseApp.getApp<RealtimeDatabase>(database);
  database.url(DATABASE_URL);

  Serial.println("Firebase initialized.");
}

// ==================================================
// Indoor Wi-Fi positioning
// ==================================================

void setupIndoorWiFi() {
  updateIndoorWiFi();
  Serial.println("Indoor Wi-Fi positioning ready.");
}

void updateIndoorWiFi() {
  if (WiFi.status() != WL_CONNECTED) {
    officeDetected = false;
    officeRSSI = -100;
    return;
  }

  String connectedSSID = WiFi.SSID();
  String connectedBSSID = WiFi.BSSIDstr();

  officeRSSI = WiFi.RSSI();

  officeDetected =
    connectedSSID == OFFICE_SSID &&
    connectedBSSID.equalsIgnoreCase(OFFICE_BSSID) &&
    officeRSSI >= OFFICE_MIN_RSSI;
}

// ==================================================
// Firebase upload
// ==================================================

void uploadNextFirebaseField() {
  int safeHeartRate =
    stableHeartRateAvailable
      ? stableHeartRate
      : 0;

  int safeSpO2 =
    stableSpO2Available
      ? stableSpO2
      : 0;

  String locationMode = "UNKNOWN";
  String indoorZone = "UNKNOWN";

  if (gpsFixAvailable) {
    locationMode = "OUTDOOR GPS";
  }
  else if (officeDetected) {
    locationMode = "INDOOR Wi-Fi";
    indoorZone = "INTERN OFFICE";
  }

  switch (firebaseUploadStep) {
    case 0:
      database.set<int>(
        firebaseClient,
        "/neuroband/health/heartRate",
        safeHeartRate,
        processFirebaseResult,
        "heartRate"
      );
      break;

    case 1:
      database.set<int>(
        firebaseClient,
        "/neuroband/health/spo2",
        safeSpO2,
        processFirebaseResult,
        "spo2"
      );
      break;

    case 2:
      database.set<bool>(
        firebaseClient,
        "/neuroband/health/fingerDetected",
        fingerDetected,
        processFirebaseResult,
        "fingerDetected"
      );
      break;

    case 3:
      database.set<bool>(
        firebaseClient,
        "/neuroband/motion/fallDetected",
        fallDetected,
        processFirebaseResult,
        "fallDetected"
      );
      break;

    case 4:
      database.set<float>(
        firebaseClient,
        "/neuroband/motion/accelerationMagnitude",
        accelerationMagnitude,
        processFirebaseResult,
        "acceleration"
      );
      break;

    case 5:
      database.set<String>(
        firebaseClient,
        "/neuroband/location/mode",
        locationMode,
        processFirebaseResult,
        "locationMode"
      );
      break;

    case 6:
      database.set<String>(
        firebaseClient,
        "/neuroband/location/indoorZone",
        indoorZone,
        processFirebaseResult,
        "indoorZone"
      );
      break;

    case 7:
      database.set<double>(
        firebaseClient,
        "/neuroband/location/latitude",
        gpsFixAvailable ? latitude : 0.0,
        processFirebaseResult,
        "latitude"
      );
      break;

    case 8:
      database.set<double>(
        firebaseClient,
        "/neuroband/location/longitude",
        gpsFixAvailable ? longitude : 0.0,
        processFirebaseResult,
        "longitude"
      );
      break;

    case 9:
      database.set<bool>(
        firebaseClient,
        "/neuroband/location/gpsFix",
        gpsFixAvailable,
        processFirebaseResult,
        "gpsFix"
      );
      break;

    case 10:
      database.set<int>(
        firebaseClient,
        "/neuroband/location/wifiRSSI",
        officeDetected ? officeRSSI : -100,
        processFirebaseResult,
        "wifiRSSI"
      );
      break;

    case 11:
      database.set<bool>(
        firebaseClient,
        "/neuroband/geofence/outsideSafeZone",
        gpsFixAvailable && outsideGeofence,
        processFirebaseResult,
        "outsideSafeZone"
      );
      break;

    case 12:
      database.set<double>(
        firebaseClient,
        "/neuroband/geofence/distanceMeters",
        gpsFixAvailable ? distanceFromSafeZone : 0.0,
        processFirebaseResult,
        "distanceMeters"
      );
      break;

    case 13:
      database.set<bool>(
        firebaseClient,
        "/neuroband/system/firebaseConnected",
        true,
        processFirebaseResult,
        "firebaseConnected"
      );
      break;

    case 14:
      database.set<unsigned long>(
        firebaseClient,
        "/neuroband/system/uptimeMillis",
        millis(),
        processFirebaseResult,
        "uptimeMillis"
      );
      break;
  }

  firebaseUploadStep++;

  if (firebaseUploadStep > 14) {
    firebaseUploadCycleActive = false;
    lastFirebaseUploadTime = millis();

    Serial.println("Firebase: Neuroband data uploaded.");
  }
}

void updateFirebase() {
  firebaseApp.loop();

  if (WiFi.status() != WL_CONNECTED) {
    officeDetected = false;
    return;
  }

  if (!firebaseApp.ready()) {
    return;
  }

  unsigned long currentTime = millis();

  if (
    !firebaseUploadCycleActive &&
    currentTime - lastFirebaseUploadTime >= FIREBASE_UPLOAD_INTERVAL
  ) {
    firebaseUploadCycleActive = true;
    firebaseUploadStep = 0;
    lastFirebaseFieldTime = 0;
  }

  if (
    firebaseUploadCycleActive &&
    (
      lastFirebaseFieldTime == 0 ||
      currentTime - lastFirebaseFieldTime >= FIREBASE_FIELD_INTERVAL
    )
  ) {
    lastFirebaseFieldTime = currentTime;
    uploadNextFirebaseField();
  }
}

// ==================================================
// MPU6050 update
// ==================================================

void updateMPU6050() {
  mpu.update();

  accX = mpu.getAccX();
  accY = mpu.getAccY();
  accZ = mpu.getAccZ();

  gyroX = mpu.getGyroX();
  gyroY = mpu.getGyroY();
  gyroZ = mpu.getGyroZ();

  accelerationMagnitude = sqrt(
    (accX * accX) +
    (accY * accY) +
    (accZ * accZ)
  );
}

// ==================================================
// Fall detection
// ==================================================

void detectFall() {
  unsigned long currentTime = millis();

  if (
    accelerationMagnitude < FREE_FALL_THRESHOLD &&
    !possibleFreeFall
  ) {
    possibleFreeFall = true;
    freeFallStartTime = currentTime;
  }

  if (possibleFreeFall) {
    if (
      accelerationMagnitude > IMPACT_THRESHOLD &&
      currentTime - freeFallStartTime <= IMPACT_TIME_LIMIT
    ) {
      fallDetected = true;
      fallAlertStartTime = currentTime;
      possibleFreeFall = false;

      Serial.println();
      Serial.println("!!! FALL DETECTED !!!");
    }

    if (
      currentTime - freeFallStartTime > IMPACT_TIME_LIMIT
    ) {
      possibleFreeFall = false;
    }
  }

  if (
    fallDetected &&
    currentTime - fallAlertStartTime > FALL_ALERT_DURATION
  ) {
    fallDetected = false;
  }
}

// ==================================================
// MAX30102 update
// ==================================================

void updateMAX30102() {
  maxSensor.check();

  while (maxSensor.available()) {
    currentRed = maxSensor.getFIFORed();
    currentIR = maxSensor.getFIFOIR();

    maxSensor.nextSample();

    fingerDetected = currentIR >= 15000;

    maxSaturated =
      currentIR >= 260000 ||
      currentRed >= 260000;

    if (!fingerDetected || maxSaturated) {
      samplesCollected = 0;
      initialBufferReady = false;

      heartRateValid = 0;
      spo2Valid = 0;

      resetStableHealthValues();
      continue;
    }

    if (!initialBufferReady) {
      redBuffer[samplesCollected] = currentRed;
      irBuffer[samplesCollected] = currentIR;

      samplesCollected++;

      if (samplesCollected >= MAX_BUFFER_LENGTH) {
        calculateHealthValues();
        updateStableHealthValues();

        initialBufferReady = true;
        samplesCollected = 0;
      }
    } else {
      int startIndex =
        MAX_BUFFER_LENGTH - NEW_SAMPLES;

      redBuffer[startIndex + samplesCollected] =
        currentRed;

      irBuffer[startIndex + samplesCollected] =
        currentIR;

      samplesCollected++;

      if (samplesCollected >= NEW_SAMPLES) {
        calculateHealthValues();
        updateStableHealthValues();
        shiftMAXBuffer();

        samplesCollected = 0;
      }
    }
  }
}

// ==================================================
// Calculate Heart Rate and SpO2
// ==================================================

void calculateHealthValues() {
  int32_t bufferLength = MAX_BUFFER_LENGTH;

  maxim_heart_rate_and_oxygen_saturation(
    irBuffer,
    bufferLength,
    redBuffer,
    &spo2,
    &spo2Valid,
    &heartRate,
    &heartRateValid
  );
}


// ==================================================
// Stable heart-rate and SpO2 filtering
// ==================================================

void resetStableHealthValues() {
  stableHeartRate = 0;
  stableSpO2 = 0;
  previousAcceptedHeartRate = 0;
  previousAcceptedSpO2 = 0;
  heartRateCandidateSum = 0;
  spo2CandidateSum = 0;
  heartRateStableCount = 0;
  spo2StableCount = 0;
  stableHeartRateAvailable = false;
  stableSpO2Available = false;
}

void updateStableHealthValues() {
  bool heartRateInRange =
    heartRateValid == 1 &&
    heartRate >= HEART_RATE_MIN &&
    heartRate <= HEART_RATE_MAX;

  bool spo2InRange =
    spo2Valid == 1 &&
    spo2 >= SPO2_MIN &&
    spo2 <= SPO2_MAX;

  if (heartRateInRange) {
    bool jumpAcceptable =
      previousAcceptedHeartRate == 0 ||
      abs(heartRate - previousAcceptedHeartRate) <= HEART_RATE_MAX_JUMP;

    if (jumpAcceptable) {
      heartRateCandidateSum += heartRate;
      heartRateStableCount++;
      previousAcceptedHeartRate = heartRate;

      if (heartRateStableCount >= REQUIRED_STABLE_READINGS) {
        stableHeartRate = heartRateCandidateSum / heartRateStableCount;
        stableHeartRateAvailable = true;
        heartRateCandidateSum = 0;
        heartRateStableCount = 0;
      }
    } else {
      heartRateCandidateSum = heartRate;
      heartRateStableCount = 1;
      previousAcceptedHeartRate = heartRate;
    }
  }
  // Ignore a single invalid heart-rate result.
  // The filter resets only when the finger is removed or the signal saturates.

  if (spo2InRange) {
    bool jumpAcceptable =
      previousAcceptedSpO2 == 0 ||
      abs(spo2 - previousAcceptedSpO2) <= SPO2_MAX_JUMP;

    if (jumpAcceptable) {
      spo2CandidateSum += spo2;
      spo2StableCount++;
      previousAcceptedSpO2 = spo2;

      if (spo2StableCount >= REQUIRED_STABLE_READINGS) {
        stableSpO2 = spo2CandidateSum / spo2StableCount;
        stableSpO2Available = true;
        spo2CandidateSum = 0;
        spo2StableCount = 0;
      }
    } else {
      spo2CandidateSum = spo2;
      spo2StableCount = 1;
      previousAcceptedSpO2 = spo2;
    }
  }
  // Ignore a single invalid SpO2 result.
  // Continue collecting the next acceptable result.
}

// ==================================================
// Shift MAX30102 buffer
// ==================================================

void shiftMAXBuffer() {
  for (
    int i = NEW_SAMPLES;
    i < MAX_BUFFER_LENGTH;
    i++
  ) {
    redBuffer[i - NEW_SAMPLES] = redBuffer[i];
    irBuffer[i - NEW_SAMPLES] = irBuffer[i];
  }
}

// ==================================================
// GPS update
// ==================================================

void updateGPS() {
  while (gpsSerial.available() > 0) {
    char incomingCharacter = gpsSerial.read();
    gps.encode(incomingCharacter);
  }

  if (
    gps.location.isValid() &&
    gps.location.age() < GPS_DATA_TIMEOUT
  ) {
    latitude = gps.location.lat();
    longitude = gps.location.lng();

    gpsFixAvailable = true;
  } else {
    gpsFixAvailable = false;
  }

  if (gps.satellites.isValid()) {
    satelliteCount = gps.satellites.value();
  }

  if (gps.hdop.isValid()) {
    gpsHDOP = gps.hdop.hdop();
  }
}

// ==================================================
// Geofence calculation
// ==================================================

void updateGeofence() {
  if (!gpsFixAvailable) {
    geofenceAvailable = false;
    outsideGeofence = false;
    distanceFromSafeZone = 0.0;
    return;
  }

  distanceFromSafeZone =
    TinyGPSPlus::distanceBetween(
      latitude,
      longitude,
      SAFE_ZONE_LATITUDE,
      SAFE_ZONE_LONGITUDE
    );

  geofenceAvailable = true;

  outsideGeofence =
    distanceFromSafeZone > SAFE_ZONE_RADIUS_METERS;
}

// ==================================================
// Display all readings
// ==================================================

void displayReadings() {
  Serial.println();
  Serial.println("========== NEUROBAND V6.2 ==========");

  // ---------------- Health ----------------

  Serial.println();
  Serial.println("HEALTH");

  Serial.print("IR          : ");
  Serial.println(currentIR);

  Serial.print("RED         : ");
  Serial.println(currentRed);

  if (!fingerDetected) {
    Serial.println("Finger      : Not detected");
    Serial.println("Heart Rate  : --");
    Serial.println("SpO2        : --");
  }
  else if (maxSaturated) {
    Serial.println("Finger      : Detected");
    Serial.println("Signal      : Saturated");
    Serial.println("Heart Rate  : --");
    Serial.println("SpO2        : --");
  }
  else {
    Serial.println("Finger      : Detected");
    Serial.println("Signal      : Good");

    if (stableHeartRateAvailable) {
      Serial.print("Heart Rate  : ");
      Serial.print(stableHeartRate);
      Serial.println(" BPM");
    } else {
      Serial.println("Heart Rate  : Stabilizing...");
    }

    if (stableSpO2Available) {
      Serial.print("SpO2        : ");
      Serial.print(stableSpO2);
      Serial.println(" %");
    } else {
      Serial.println("SpO2        : Stabilizing...");
    }
  }

  // ---------------- Motion ----------------

  Serial.println();
  Serial.println("MOTION");

  Serial.print("ACC X: ");
  Serial.print(accX, 2);

  Serial.print(" | Y: ");
  Serial.print(accY, 2);

  Serial.print(" | Z: ");
  Serial.println(accZ, 2);

  Serial.print("Acceleration magnitude: ");
  Serial.print(accelerationMagnitude, 2);
  Serial.println(" g");

  Serial.print("GYRO X: ");
  Serial.print(gyroX, 2);

  Serial.print(" | Y: ");
  Serial.print(gyroY, 2);

  Serial.print(" | Z: ");
  Serial.println(gyroZ, 2);

  if (fallDetected) {
    Serial.println("Fall Status : FALL DETECTED");
  }
  else if (possibleFreeFall) {
    Serial.println("Fall Status : Checking impact...");
  }
  else {
    Serial.println("Fall Status : Normal");
  }

  // ---------------- Location ----------------

  Serial.println();
  Serial.println("LOCATION");

  Serial.print("GPS characters received: ");
  Serial.println(gps.charsProcessed());

  if (gpsFixAvailable) {
    Serial.println("Location Mode : OUTDOOR GPS");

    Serial.print("Latitude      : ");
    Serial.println(latitude, 6);

    Serial.print("Longitude     : ");
    Serial.println(longitude, 6);

    Serial.print("Satellites    : ");
    Serial.println(satelliteCount);

    Serial.print("HDOP          : ");
    Serial.println(gpsHDOP, 2);

    Serial.println("GPS Fix       : Available");
  }
  else if (officeDetected) {
    Serial.println("Location Mode : INDOOR Wi-Fi");
    Serial.println("Indoor Zone   : INTERN OFFICE");

    Serial.print("Office SSID   : ");
    Serial.println(OFFICE_SSID);

    Serial.print("Signal RSSI   : ");
    Serial.print(officeRSSI);
    Serial.println(" dBm");

    if (officeRSSI >= -60) {
      Serial.println("Signal Level  : Strong");
    }
    else if (officeRSSI >= -70) {
      Serial.println("Signal Level  : Medium");
    }
    else {
      Serial.println("Signal Level  : Weak");
    }

    Serial.println("GPS Fix       : Not available");
  }
  else {
    Serial.println("Location Mode : UNKNOWN");
    Serial.println("Indoor Zone   : UNKNOWN");

    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("Wi-Fi Status  : Connected, office BSSID not matched");
    } else {
      Serial.println("Wi-Fi Status  : Disconnected");
    }

    Serial.print("Satellites    : ");
    Serial.println(satelliteCount);

    Serial.println("GPS Fix       : Not available");
  }

  // ---------------- Geofence ----------------

  Serial.println();
  Serial.println("GEOFENCE");

  Serial.print("Safe latitude : ");
  Serial.println(SAFE_ZONE_LATITUDE, 6);

  Serial.print("Safe longitude: ");
  Serial.println(SAFE_ZONE_LONGITUDE, 6);

  Serial.print("Safe radius   : ");
  Serial.print(SAFE_ZONE_RADIUS_METERS, 1);
  Serial.println(" m");

  if (!gpsFixAvailable) {
    Serial.println("Geofence      : Inactive indoors");
    Serial.println("Distance      : Waiting for GPS...");
    Serial.println("Zone Status   : Using indoor Wi-Fi zone");
  }
  else if (!geofenceAvailable) {
    Serial.println("Distance      : Waiting for GPS...");
    Serial.println("Zone Status   : Unknown");
  } else {
    Serial.print("Distance      : ");
    Serial.print(distanceFromSafeZone, 2);
    Serial.println(" m");

    if (outsideGeofence) {
      Serial.println("Zone Status   : OUTSIDE SAFE ZONE");
      Serial.println("Alert         : GEOFENCE ALERT");
    } else {
      Serial.println("Zone Status   : INSIDE SAFE ZONE");
      Serial.println("Alert         : Normal");
    }
  }

  Serial.println();
  Serial.println("==================================");
}

// ==================================================
// Arduino setup
// ==================================================

void setup() {
  Serial.begin(115200);
  delay(1500);

  Serial.println();
  Serial.println("Starting Neuroband V6.2...");

  if (!setupMAX30102()) {
    Serial.println("MAX30102 NOT FOUND");
    Serial.println(
      "Check GPIO21, GPIO22, 3.3V and GND."
    );

    while (true) {
      delay(1000);
    }
  }

  Serial.println("MAX30102 READY");

  setupMPU6050();
  Serial.println("MPU6050 READY");

  setupGPS();
  Serial.println("GPS READY");

  connectWiFi();

  setupIndoorWiFi();
  Serial.println("INDOOR WI-FI READY");

  setupFirebase();
  Serial.println("FIREBASE READY");

  Serial.println();
  Serial.println(
    "Neuroband V6.2 with dashboard heartbeat and Firebase started."
  );
}

// ==================================================
// Arduino loop
// ==================================================

void loop() {
  updateMPU6050();
  detectFall();

  updateMAX30102();
  updateGPS();
  updateIndoorWiFi();
  updateGeofence();
  updateFirebase();

  if (
    millis() - lastDisplayTime >= DISPLAY_INTERVAL
  ) {
    lastDisplayTime = millis();
    displayReadings();
  }

  delay(1);
}