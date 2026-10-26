import { initializeApp } from "https://www.gstatic.com/firebasejs/12.6.0/firebase-app.js";
import { getDatabase, ref, onValue } from "https://www.gstatic.com/firebasejs/12.6.0/firebase-database.js";

const firebaseConfig = {
  apiKey: "AIzaSyC-tJuoXa8kC39rJg240zEYkKNdld_qMvM",
  authDomain: "neuroband-502a6.firebaseapp.com",
  databaseURL: "https://neuroband-502a6-default-rtdb.asia-southeast1.firebasedatabase.app",
  projectId: "neuroband-502a6",
  storageBucket: "neuroband-502a6.firebasestorage.app",
  messagingSenderId: "782044609121",
  appId: "1:782044609121:web:bb00e6d904073e98574820"
};

const SAFE_ZONE = {
  latitude: 9.956748,
  longitude: 78.151152,
  radiusMeters: 5
};

const OFFLINE_AFTER_MS = 15000;
const HEART_RATE_LOW = 50;
const HEART_RATE_HIGH = 120;
const SPO2_LOW = 90;

const app = initializeApp(firebaseConfig);
const database = getDatabase(app);

const byId = (id) => document.getElementById(id);

const ui = {
  deviceDot: byId("deviceDot"),
  deviceStatus: byId("deviceStatus"),
  lastUpdated: byId("lastUpdated"),
  soundButton: byId("soundButton"),
  alarmPanel: byId("alarmPanel"),
  alarmList: byId("alarmList"),
  acknowledgeButton: byId("acknowledgeButton"),
  muteButton: byId("muteButton"),

  heartCard: byId("heartCard"),
  heartIcon: byId("heartIcon"),
  heartRate: byId("heartRate"),
  heartStatus: byId("heartStatus"),

  spo2Card: byId("spo2Card"),
  spo2Gauge: byId("spo2Gauge"),
  spo2: byId("spo2"),
  spo2Status: byId("spo2Status"),

  fallCard: byId("fallCard"),
  fallValue: byId("fallValue"),
  fallBadge: byId("fallBadge"),

  deviceCard: byId("deviceCard"),
  fingerValue: byId("fingerValue"),
  fingerBadge: byId("fingerBadge"),

  wifiIcon: byId("wifiIcon"),
  wifiStatus: byId("wifiStatus"),
  wifiDetails: byId("wifiDetails"),

  gpsIcon: byId("gpsIcon"),
  gpsStatus: byId("gpsStatus"),
  gpsDetails: byId("gpsDetails"),

  firebaseIcon: byId("firebaseIcon"),
  firebaseStatus: byId("firebaseStatus"),

  zoneStatus: byId("zoneStatus"),
  locationDescription: byId("locationDescription"),
  locationMode: byId("locationMode"),
  indoorZone: byId("indoorZone"),
  distanceValue: byId("distanceValue")
};

let latestData = {};
let lastDeviceUpdateAt = 0;
let lastHeartbeatValue = null;
let firebaseConnected = false;
let alarmAcknowledged = false;
let soundEnabled = false;
let soundMuted = false;
let alarmTimer = null;
let audioContext = null;

const map = L.map("map", {
  zoomControl: true,
  attributionControl: true
}).setView([SAFE_ZONE.latitude, SAFE_ZONE.longitude], 18);

L.tileLayer("https://tile.openstreetmap.org/{z}/{x}/{y}.png", {
  maxZoom: 20,
  attribution: '&copy; <a href="https://www.openstreetmap.org/copyright">OpenStreetMap</a> contributors'
}).addTo(map);

const safeCentreMarker = L.circleMarker(
  [SAFE_ZONE.latitude, SAFE_ZONE.longitude],
  {
    radius: 7,
    color: "#11603d",
    fillColor: "#39d98a",
    fillOpacity: 1,
    weight: 3
  }
).addTo(map).bindPopup("Configured safe-zone centre");

const safeZoneCircle = L.circle(
  [SAFE_ZONE.latitude, SAFE_ZONE.longitude],
  {
    radius: SAFE_ZONE.radiusMeters,
    color: "#54a8ff",
    fillColor: "#54a8ff",
    fillOpacity: 0.12,
    weight: 3
  }
).addTo(map);

let patientMarker = null;

function setClass(element, baseClass, stateClass) {
  element.className = `${baseClass} ${stateClass}`;
}

function formatElapsed(milliseconds) {
  const seconds = Math.max(0, Math.floor(milliseconds / 1000));

  if (seconds < 2) return "Updated just now";
  if (seconds < 60) return `Last Updated: ${seconds} seconds ago`;

  const minutes = Math.floor(seconds / 60);
  return `Last Updated: ${minutes} minute${minutes === 1 ? "" : "s"} ago`;
}

function isDeviceOnline() {
  return lastDeviceUpdateAt > 0 &&
    Date.now() - lastDeviceUpdateAt <= OFFLINE_AFTER_MS;
}

function updateHeader() {
  const online = isDeviceOnline();

  setClass(ui.deviceDot, "indicator", online ? "online" : "offline");
  ui.deviceStatus.textContent = online ? "Device Online" : "Device Offline";

  if (lastDeviceUpdateAt === 0) {
    ui.lastUpdated.textContent = "Waiting for device data";
  } else {
    ui.lastUpdated.textContent = formatElapsed(Date.now() - lastDeviceUpdateAt);
  }

  setClass(
    ui.firebaseIcon,
    "connectivity-icon",
    firebaseConnected ? "good" : "danger"
  );
  ui.firebaseStatus.textContent =
    firebaseConnected ? "Connected" : "Disconnected";
}

function updateHeartAndSpO2(health = {}) {
  const fingerDetected = Boolean(health.fingerDetected);
  const heartRate = Number(health.heartRate || 0);
  const spo2 = Number(health.spo2 || 0);

  ui.fingerValue.textContent =
    fingerDetected ? "Device Attached" : "Device Detached";
  ui.fingerBadge.textContent =
    fingerDetected ? "Finger detected" : "No finger detected";
  setClass(
    ui.fingerBadge,
    "badge",
    fingerDetected ? "good" : "danger"
  );

  ui.heartRate.textContent =
    fingerDetected && heartRate > 0 ? heartRate : "--";

  ui.heartIcon.classList.toggle(
    "beating",
    fingerDetected && heartRate > 0
  );

  if (!fingerDetected) {
    ui.heartStatus.textContent = "Device detached";
  } else if (heartRate === 0) {
    ui.heartStatus.textContent = "Stabilizing reading";
  } else if (heartRate < HEART_RATE_LOW) {
    ui.heartStatus.textContent = "Low heart rate";
  } else if (heartRate > HEART_RATE_HIGH) {
    ui.heartStatus.textContent = "High heart rate";
  } else {
    ui.heartStatus.textContent = "Normal range";
  }

  ui.spo2.textContent =
    fingerDetected && spo2 > 0 ? spo2 : "--";

  const gaugeDegrees =
    fingerDetected && spo2 > 0
      ? Math.min(360, Math.max(0, spo2 * 3.6))
      : 0;

  ui.spo2Gauge.style.setProperty(
    "--gauge-value",
    `${gaugeDegrees}deg`
  );

  if (!fingerDetected) {
    ui.spo2Status.textContent = "Device detached";
  } else if (spo2 === 0) {
    ui.spo2Status.textContent = "Stabilizing reading";
  } else if (spo2 < SPO2_LOW) {
    ui.spo2Status.textContent = "Low blood oxygen";
  } else if (spo2 < 95) {
    ui.spo2Status.textContent = "Observe reading";
  } else {
    ui.spo2Status.textContent = "Normal range";
  }
}

function updateFall(motion = {}) {
  const fallDetected = Boolean(motion.fallDetected);

  ui.fallValue.textContent =
    fallDetected ? "FALL DETECTED" : "Normal";
  ui.fallBadge.textContent =
    fallDetected ? "Immediate attention required" : "No fall detected";

  setClass(
    ui.fallBadge,
    "badge",
    fallDetected ? "danger" : "good"
  );

  ui.fallCard.classList.toggle("fall-alert", fallDetected);
}

function updateConnectivity(location = {}) {
  const mode = location.mode || "UNKNOWN";
  const indoorZone = location.indoorZone || "UNKNOWN";
  const wifiRSSI = Number(location.wifiRSSI ?? -100);
  const gpsFix = Boolean(location.gpsFix);

  const wifiAvailable =
    mode === "INDOOR Wi-Fi" &&
    indoorZone !== "UNKNOWN" &&
    wifiRSSI > -100;

  setClass(
    ui.wifiIcon,
    "connectivity-icon",
    wifiAvailable ? "good" : "danger"
  );

  ui.wifiStatus.textContent =
    wifiAvailable ? "Connected" : "Unavailable";

  ui.wifiDetails.textContent =
    wifiAvailable
      ? `${indoorZone} · ${wifiRSSI} dBm`
      : "Known indoor zone not detected";

  setClass(
    ui.gpsIcon,
    "connectivity-icon",
    gpsFix ? "good" : "warning"
  );

  ui.gpsStatus.textContent =
    gpsFix ? "GPS Fix Available" : "GPS Fix Unavailable";

  ui.gpsDetails.textContent =
    gpsFix ? "Outdoor positioning active" : "Indoor Wi-Fi mode active";
}

function updateMap(location = {}, geofence = {}) {
  const mode = location.mode || "UNKNOWN";
  const indoorZone = location.indoorZone || "UNKNOWN";
  const gpsFix = Boolean(location.gpsFix);
  const latitude = Number(location.latitude || 0);
  const longitude = Number(location.longitude || 0);
  const distanceMeters = Number(geofence.distanceMeters || 0);
  const outsideSafeZone = Boolean(geofence.outsideSafeZone);

  ui.locationMode.textContent = mode;
  ui.indoorZone.textContent = indoorZone;
  ui.distanceValue.textContent =
    gpsFix ? `${distanceMeters.toFixed(2)} m` : "-- m";

  if (gpsFix && latitude !== 0 && longitude !== 0) {
    const position = [latitude, longitude];

    if (!patientMarker) {
      patientMarker = L.circleMarker(position, {
        radius: 8,
        color: "#7e1b25",
        fillColor: "#ff5c68",
        fillOpacity: 1,
        weight: 3
      }).addTo(map).bindPopup("Current patient location");
    } else {
      patientMarker.setLatLng(position);
    }

    const bounds = L.latLngBounds([
      position,
      [SAFE_ZONE.latitude, SAFE_ZONE.longitude]
    ]);

    map.fitBounds(bounds.pad(0.45), { maxZoom: 20 });

    if (outsideSafeZone) {
      ui.zoneStatus.textContent = "OUTSIDE SAFE ZONE";
      ui.locationDescription.textContent =
        "The patient is outside the configured geofence.";
    } else {
      ui.zoneStatus.textContent = "Inside Safe Zone";
      ui.locationDescription.textContent =
        "The patient is within the configured geofence.";
    }
  } else {
    if (patientMarker) {
      map.removeLayer(patientMarker);
      patientMarker = null;
    }

    map.setView(
      [SAFE_ZONE.latitude, SAFE_ZONE.longitude],
      18
    );

    ui.zoneStatus.textContent =
      mode === "INDOOR Wi-Fi"
        ? `Indoor Location: ${indoorZone}`
        : "Waiting for GPS fix";

    ui.locationDescription.textContent =
      mode === "INDOOR Wi-Fi"
        ? "GPS geofencing is inactive indoors; the known Wi-Fi zone is being used."
        : "The map is centred on the configured safe zone.";
  }
}

function collectAlerts(data) {
  const health = data.health || {};
  const motion = data.motion || {};
  const geofence = data.geofence || {};

  const alerts = [];
  const online = isDeviceOnline();
  const fingerDetected = Boolean(health.fingerDetected);
  const heartRate = Number(health.heartRate || 0);
  const spo2 = Number(health.spo2 || 0);

  if (!online) {
    alerts.push("Device offline: no fresh wearable data received.");
  }

  if (Boolean(motion.fallDetected)) {
    alerts.push("Fall detected: check the patient immediately.");
  }

  if (Boolean(geofence.outsideSafeZone)) {
    alerts.push("Geofence alert: patient is outside the safe zone.");
  }

  if (!fingerDetected && online) {
    alerts.push("Device detached: no finger detected on the sensor.");
  }

  if (fingerDetected && heartRate > 0 && heartRate < HEART_RATE_LOW) {
    alerts.push(`Low heart rate detected: ${heartRate} BPM.`);
  }

  if (fingerDetected && heartRate > HEART_RATE_HIGH) {
    alerts.push(`High heart rate detected: ${heartRate} BPM.`);
  }

  if (fingerDetected && spo2 > 0 && spo2 < SPO2_LOW) {
    alerts.push(`Low SpO₂ detected: ${spo2}%.`);
  }

  return alerts;
}

function createAudioContext() {
  if (!audioContext) {
    audioContext = new (
      window.AudioContext || window.webkitAudioContext
    )();
  }

  if (audioContext.state === "suspended") {
    audioContext.resume();
  }
}

function playAlarmTone() {
  if (!soundEnabled || soundMuted || !audioContext) {
    return;
  }

  const oscillator = audioContext.createOscillator();
  const gain = audioContext.createGain();

  oscillator.type = "square";
  oscillator.frequency.setValueAtTime(
    880,
    audioContext.currentTime
  );

  gain.gain.setValueAtTime(0.0001, audioContext.currentTime);
  gain.gain.exponentialRampToValueAtTime(
    0.12,
    audioContext.currentTime + 0.02
  );
  gain.gain.exponentialRampToValueAtTime(
    0.0001,
    audioContext.currentTime + 0.25
  );

  oscillator.connect(gain);
  gain.connect(audioContext.destination);

  oscillator.start();
  oscillator.stop(audioContext.currentTime + 0.28);
}

function stopAlarmTimer() {
  if (alarmTimer) {
    clearInterval(alarmTimer);
    alarmTimer = null;
  }
}

function updateAlarmPanel() {
  const alerts = collectAlerts(latestData);

  if (alerts.length === 0 || alarmAcknowledged) {
    ui.alarmPanel.classList.add("hidden");
    stopAlarmTimer();
    return;
  }

  ui.alarmPanel.classList.remove("hidden");
  ui.alarmList.replaceChildren();

  alerts.forEach((alertText) => {
    const item = document.createElement("div");
    item.textContent = `• ${alertText}`;
    ui.alarmList.appendChild(item);
  });

  if (!alarmTimer && soundEnabled && !soundMuted) {
    playAlarmTone();
    alarmTimer = setInterval(playAlarmTone, 1800);
  }
}

function render(data) {
  latestData = data || {};

  updateHeartAndSpO2(latestData.health);
  updateFall(latestData.motion);
  updateConnectivity(latestData.location);
  updateMap(latestData.location, latestData.geofence);
  updateHeader();
  updateAlarmPanel();
}

ui.soundButton.addEventListener("click", () => {
  createAudioContext();
  soundEnabled = true;
  soundMuted = false;
  ui.soundButton.textContent = "Alarm Sound Enabled";
  updateAlarmPanel();
});

ui.muteButton.addEventListener("click", () => {
  soundMuted = !soundMuted;
  ui.muteButton.textContent = soundMuted ? "Unmute" : "Mute";

  if (soundMuted) {
    stopAlarmTimer();
  } else {
    updateAlarmPanel();
  }
});

ui.acknowledgeButton.addEventListener("click", () => {
  alarmAcknowledged = true;
  stopAlarmTimer();
  updateAlarmPanel();
});

onValue(ref(database, ".info/connected"), (snapshot) => {
  firebaseConnected = snapshot.val() === true;
  updateHeader();
});

onValue(
  ref(database, "neuroband"),
  (snapshot) => {
    const nextData = snapshot.val() || {};
    const heartbeatValue =
      nextData.system?.uptimeMillis ??
      JSON.stringify(nextData);

    if (heartbeatValue !== lastHeartbeatValue) {
      lastHeartbeatValue = heartbeatValue;
      lastDeviceUpdateAt = Date.now();
      alarmAcknowledged = false;
    }

    render(nextData);
  },
  (error) => {
    console.error("Firebase read failed:", error);
    firebaseConnected = false;
    updateHeader();
  }
);

setInterval(() => {
  updateHeader();
  updateAlarmPanel();
}, 1000);

setTimeout(() => map.invalidateSize(), 300);
