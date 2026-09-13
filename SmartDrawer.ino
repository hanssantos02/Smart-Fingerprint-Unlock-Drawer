#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Adafruit_Fingerprint.h>
#include <Preferences.h>
#include <vector>
#include "time.h"

// -------------------------------------------------------------
// 1. NETWORK CREDENTIALS
// -------------------------------------------------------------
const char* ssid     = "YOUR WIFI NAME HERE";
const char* password = "YOUR WIFI PASSWORD HERE";
const char* hostName = "smartdrawer";

const char* ntpServer = "pool.ntp.org";
const long  gmtOffset_sec = 8 * 3600;
const int   daylightOffset_sec = 0;

// -------------------------------------------------------------
// 2. HARDWARE PINS
// -------------------------------------------------------------
#define BUZZER_PIN 25
#define LOCK_PIN   32
#define BUTTON_PIN 14

Adafruit_Fingerprint finger = Adafruit_Fingerprint(&Serial2);
WebServer server(80);
Preferences prefs;

// -------------------------------------------------------------
// 3. SYSTEM STATE, LOGS & ACTIVE USERS CACHE
// -------------------------------------------------------------
bool isUnlocked = false;
unsigned long unlockStartTime = 0;
const unsigned long UNLOCK_DURATION = 3000;

// Fast RAM Cache for Enrolled IDs (Solves the CPU bottleneck!)
std::vector<int> enrolledIDs;

struct LogEntry {
  String timestamp;
  String message;
  String badgeClass;
};

const int MAX_LOGS = 10;
LogEntry accessLogs[MAX_LOGS];
int logCount = 0;

// -------------------------------------------------------------
// 4. CACHE & NAME MANAGEMENT
// -------------------------------------------------------------
void refreshEnrolledCache() {
  enrolledIDs.clear();
  Serial.print(">> Indexing enrolled fingerprints... ");
  for (int id = 1; id <= 300; id++) {
    if (finger.loadModel(id) == FINGERPRINT_OK) {
      enrolledIDs.push_back(id);
    }
  }
  Serial.printf("Done! Found %d users.\n", enrolledIDs.size());
}

String getUserName(int id) {
  prefs.begin("user_names", true);
  String key = "id_" + String(id);
  String name = prefs.getString(key.c_str(), "User #" + String(id));
  prefs.end();
  return name;
}

void setUserName(int id, String name) {
  prefs.begin("user_names", false);
  String key = "id_" + String(id);
  prefs.putString(key.c_str(), name);
  prefs.end();
}

void removeUserName(int id) {
  prefs.begin("user_names", false);
  String key = "id_" + String(id);
  prefs.remove(key.c_str());
  prefs.end();
}

// -------------------------------------------------------------
// 5. SOUNDS & HELPERS
// -------------------------------------------------------------
String getCurrentTime() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return "Time Syncing...";
  char timeBuff[30];
  strftime(timeBuff, sizeof(timeBuff), "%I:%M:%S %p", &timeinfo);
  return String(timeBuff);
}

void addLog(String message, String badgeClass) {
  if (logCount < MAX_LOGS) {
    accessLogs[logCount] = { getCurrentTime(), message, badgeClass };
    logCount++;
  } else {
    for (int i = 0; i < MAX_LOGS - 1; i++) {
      accessLogs[i] = accessLogs[i + 1];
    }
    accessLogs[MAX_LOGS - 1] = { getCurrentTime(), message, badgeClass };
  }
}

void beepShort() {
  digitalWrite(BUZZER_PIN, HIGH); delay(80); digitalWrite(BUZZER_PIN, LOW);
}
void beepSuccess() {
  digitalWrite(BUZZER_PIN, HIGH); delay(150); digitalWrite(BUZZER_PIN, LOW);
}
void beepSuccessLong() {
  digitalWrite(BUZZER_PIN, HIGH); delay(400); digitalWrite(BUZZER_PIN, LOW);
}
void beepDenied() {
  for (int i = 0; i < 3; i++) {
    digitalWrite(BUZZER_PIN, HIGH); delay(80);
    digitalWrite(BUZZER_PIN, LOW);  delay(80);
  }
}
void beepAdminMode() {
  for (int i = 0; i < 2; i++) {
    digitalWrite(BUZZER_PIN, HIGH); delay(100);
    digitalWrite(BUZZER_PIN, LOW);  delay(100);
  }
}

void triggerUnlock(String source) {
  isUnlocked = true;
  unlockStartTime = millis();
  digitalWrite(LOCK_PIN, HIGH);
  beepSuccess();
  addLog("Unlocked: " + source, "success");
}

// -------------------------------------------------------------
// 6. MOBILE-FIRST HTML DASHBOARD
// -------------------------------------------------------------
void handleRoot() {
  String html = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0, maximum-scale=1.0, user-scalable=no">
  <title>Smart Drawer Lock</title>
  <style>
    *, *::before, *::after { box-sizing: border-box; margin: 0; padding: 0; }
    body {
      background-color: #0b1120;
      color: #f1f5f9;
      font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif;
      padding: 14px 10px;
      display: flex;
      justify-content: center;
      overflow-x: hidden;
    }
    .app-shell { width: 100%; max-width: 440px; display: flex; flex-direction: column; gap: 14px; }
    .card {
      background: #1e293b;
      border: 1px solid rgba(255, 255, 255, 0.08);
      border-radius: 18px;
      padding: 16px;
      box-shadow: 0 6px 20px rgba(0, 0, 0, 0.35);
    }
    .app-header { text-align: center; }
    .app-title { font-size: 1.25rem; font-weight: 700; color: #38bdf8; }
    .app-subtitle { font-size: 0.75rem; color: #64748b; margin-top: 2px; }

    .status-pill {
      display: flex; align-items: center; justify-content: center; gap: 8px;
      padding: 16px; border-radius: 14px; font-size: 1.2rem; font-weight: 700;
      margin: 12px 0; transition: all 0.3s ease;
    }
    .status-locked {
      background: linear-gradient(135deg, rgba(239, 68, 68, 0.15), rgba(185, 28, 28, 0.25));
      color: #f87171; border: 1px solid rgba(239, 68, 68, 0.35);
      box-shadow: 0 0 16px rgba(239, 68, 68, 0.15);
    }
    .status-unlocked {
      background: linear-gradient(135deg, rgba(34, 197, 94, 0.15), rgba(21, 128, 61, 0.25));
      color: #4ade80; border: 1px solid rgba(34, 197, 94, 0.35);
      box-shadow: 0 0 20px rgba(34, 197, 94, 0.25);
    }

    .btn-unlock {
      width: 100%; padding: 14px; border-radius: 14px; border: none;
      background: linear-gradient(135deg, #0284c7, #2563eb); color: #fff;
      font-size: 1rem; font-weight: 600; cursor: pointer;
      box-shadow: 0 4px 14px rgba(2, 132, 199, 0.35);
    }
    .btn-unlock:active { transform: scale(0.98); }

    .metrics { display: grid; grid-template-columns: 1fr 1fr; gap: 10px; margin-top: 12px; }
    .metric-box { background: #0f172a; padding: 10px; border-radius: 12px; text-align: center; border: 1px solid rgba(255, 255, 255, 0.04); }
    .metric-value { font-size: 1.1rem; font-weight: 700; color: #38bdf8; }
    .metric-label { font-size: 0.7rem; color: #94a3b8; margin-top: 2px; text-transform: uppercase; }

    .section-title { font-size: 0.95rem; font-weight: 700; color: #e2e8f0; margin-bottom: 12px; }

    .user-list { display: flex; flex-direction: column; gap: 8px; }
    .user-item {
      display: grid;
      grid-template-columns: auto 1fr auto auto;
      align-items: center;
      gap: 6px;
      background: #0f172a;
      padding: 8px 10px;
      border-radius: 12px;
      border: 1px solid rgba(255, 255, 255, 0.05);
    }
    .id-tag {
      background: rgba(56, 189, 248, 0.15); color: #38bdf8;
      font-size: 0.75rem; font-weight: 700; padding: 4px 6px; border-radius: 6px;
    }
    .user-input {
      width: 100%; min-width: 0; background: #1e293b; border: 1px solid #334155;
      color: #f8fafc; padding: 6px 8px; border-radius: 8px; font-size: 0.85rem; outline: none;
    }
    .user-input:focus { border-color: #38bdf8; }
    
    .btn-sm {
      border: none; padding: 6px 10px; border-radius: 8px;
      font-size: 0.75rem; font-weight: 600; cursor: pointer;
    }
    .btn-save { background: #0284c7; color: white; }
    .btn-del { background: rgba(239, 68, 68, 0.2); color: #f87171; border: 1px solid rgba(239, 68, 68, 0.3); }

    .feed-list { display: flex; flex-direction: column; gap: 8px; }
    .feed-item {
      display: flex; align-items: center; justify-content: space-between;
      padding: 8px 10px; background: #0f172a; border-radius: 10px;
      border: 1px solid rgba(255, 255, 255, 0.04); font-size: 0.8rem;
    }
    .feed-time { color: #64748b; font-size: 0.7rem; white-space: nowrap; margin-right: 8px; }
    .badge { padding: 3px 6px; border-radius: 6px; font-size: 0.7rem; font-weight: 600; }
    .badge-success { background: rgba(34, 197, 94, 0.18); color: #4ade80; }
    .badge-danger { background: rgba(239, 68, 68, 0.18); color: #f87171; }
    .badge-warning { background: rgba(245, 158, 11, 0.18); color: #fbbf24; }
  </style>
</head>
<body>
  <div class="app-shell">
    <div class="card app-header">
      <div class="app-title">Smart Drawer Lock</div>
      <div class="app-subtitle">IoT Biometric Security System</div>
      
      <div id="lockStatus" class="status-pill status-locked">LOCKED</div>
      <button class="btn-unlock" onclick="remoteUnlock()">Remote Web Unlock</button>
      
      <div class="metrics">
        <div class="metric-box">
          <div class="metric-value" id="userCount">--</div>
          <div class="metric-label">Registered Users</div>
        </div>
        <div class="metric-box">
          <div class="metric-value" id="wifiRSSI">-- dBm</div>
          <div class="metric-label">Signal Strength</div>
        </div>
      </div>
    </div>

    <!-- USER PROFILES -->
    <div class="card">
      <div class="section-title">Registered User Profiles</div>
      <div class="user-list" id="userList">
        <p style="color:#64748b; font-size:0.8rem; text-align:center;">Loading profiles...</p>
      </div>
    </div>

    <!-- ACTIVITY FEED -->
    <div class="card">
      <div class="section-title">Live Security Activity</div>
      <div class="feed-list" id="logBody">
        <p style="color:#64748b; font-size:0.8rem; text-align:center;">No activity logged yet.</p>
      </div>
    </div>
  </div>

  <script>
    let isEditing = false;

    function updateDashboard() {
      fetch('/api/status')
        .then(res => res.json())
        .then(data => {
          const statusBox = document.getElementById('lockStatus');
          if (data.unlocked) {
            statusBox.className = "status-pill status-unlocked";
            statusBox.innerText = "UNLOCKED";
          } else {
            statusBox.className = "status-pill status-locked";
            statusBox.innerText = "LOCKED";
          }

          document.getElementById('userCount').innerText = data.users;
          document.getElementById('wifiRSSI').innerText = data.rssi + " dBm";

          if (!isEditing) {
            let userHtml = "";
            if (data.userProfiles.length === 0) {
              userHtml = "<p style='color:#64748b; font-size:0.8rem; text-align:center;'>No fingerprints enrolled.</p>";
            } else {
              data.userProfiles.forEach(u => {
                userHtml += `
                <div class="user-item">
                  <span class="id-tag">ID #${u.id}</span>
                  <input type="text" class="user-input" id="nameInput_${u.id}" value="${u.name}" 
                         onfocus="isEditing=true" onblur="setTimeout(() => isEditing=false, 300)">
                  <button class="btn-sm btn-save" onclick="saveName(${u.id})">Save</button>
                  <button class="btn-sm btn-del" onclick="deleteUser(${u.id}, '${u.name}')">🗑️</button>
                </div>`;
              });
            }
            document.getElementById('userList').innerHTML = userHtml;
          }

          let logHtml = "";
          data.logs.forEach(log => {
            logHtml += `
            <div class="feed-item">
              <span class="badge badge-${log.type}">${log.msg}</span>
              <span class="feed-time">${log.time}</span>
            </div>`;
          });
          document.getElementById('logBody').innerHTML = logHtml || "<p style='color:#64748b; font-size:0.8rem; text-align:center;'>No activity logged yet.</p>";
        });
    }

    function saveName(id) {
      const input = document.getElementById('nameInput_' + id);
      const newName = input.value.trim();
      if (!newName) return;
      fetch(`/api/rename?id=${id}&name=${encodeURIComponent(newName)}`, { method: 'POST' })
        .then(() => { isEditing = false; updateDashboard(); });
    }

    function deleteUser(id, name) {
      if (confirm(`Delete ${name} (ID #${id}) from memory?`)) {
        fetch(`/api/delete?id=${id}`, { method: 'POST' })
          .then(() => { isEditing = false; updateDashboard(); });
      }
    }

    function remoteUnlock() {
      fetch('/api/unlock', { method: 'POST' }).then(() => updateDashboard());
    }

    setInterval(updateDashboard, 2000); // Friendly 2-second poll
    updateDashboard();
  </script>
</body>
</html>
)rawliteral";
  server.send(200, "text/html", html);
}

// Ultra-fast cached API handler (Takes 1ms instead of 4,000ms!)
void handleApiStatus() {
  String json = "{";
  json += "\"unlocked\":" + String(isUnlocked ? "true" : "false") + ",";
  json += "\"users\":" + String(enrolledIDs.size()) + ",";
  json += "\"rssi\":" + String(WiFi.RSSI()) + ",";
  
  json += "\"userProfiles\":[";
  for (size_t i = 0; i < enrolledIDs.size(); i++) {
    int id = enrolledIDs[i];
    if (i > 0) json += ",";
    json += "{\"id\":" + String(id) + ",\"name\":\"" + getUserName(id) + "\"}";
  }
  json += "],";

  json += "\"logs\":[";
  for (int i = logCount - 1; i >= 0; i--) {
    json += "{";
    json += "\"time\":\"" + accessLogs[i].timestamp + "\",";
    json += "\"msg\":\"" + accessLogs[i].message + "\",";
    json += "\"type\":\"" + accessLogs[i].badgeClass + "\"";
    json += "}";
    if (i > 0) json += ",";
  }
  json += "]}";
  
  server.send(200, "application/json", json);
}

void handleApiRename() {
  if (server.hasArg("id") && server.hasArg("name")) {
    int id = server.arg("id").toInt();
    String name = server.arg("name");
    setUserName(id, name);
    addLog("Renamed: ID #" + String(id) + " to " + name, "warning");
    server.send(200, "text/plain", "OK");
  } else {
    server.send(400, "text/plain", "Missing Args");
  }
}

void handleApiDelete() {
  if (server.hasArg("id")) {
    int id = server.arg("id").toInt();
    String name = getUserName(id);

    if (finger.deleteModel(id) == FINGERPRINT_OK) {
      removeUserName(id);
      
      // Remove from RAM cache immediately
      for (auto it = enrolledIDs.begin(); it != enrolledIDs.end(); ++it) {
        if (*it == id) { enrolledIDs.erase(it); break; }
      }

      addLog("Deleted: " + name + " (ID #" + String(id) + ")", "danger");
      beepShort();
      server.send(200, "text/plain", "OK");
    } else {
      server.send(500, "text/plain", "Hardware Delete Failed");
    }
  } else {
    server.send(400, "text/plain", "Missing ID");
  }
}

void handleApiUnlock() {
  triggerUnlock("Web Remote");
  server.send(200, "text/plain", "OK");
}

// -------------------------------------------------------------
// 7. SETUP
// -------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  while (!Serial);
  delay(1000);

  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(LOCK_PIN, OUTPUT);
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  digitalWrite(BUZZER_PIN, LOW);
  digitalWrite(LOCK_PIN, LOW);

  Serial2.begin(57600, SERIAL_8N1, 26, 27);
  if (!finger.verifyPassword()) {
    while (1) { delay(1); }
  }

  // Build the instant RAM cache on boot
  refreshEnrolledCache();

  WiFi.begin(ssid, password);
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 25) {
    delay(500);
    Serial.print(".");
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    if (MDNS.begin(hostName)) {
      Serial.println(">> mDNS Started! Access at: http://smartdrawer.local");
    }
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
    addLog("System Online (Wi-Fi)", "warning");
    beepSuccess();
  }

  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/status", HTTP_GET, handleApiStatus);
  server.on("/api/rename", HTTP_POST, handleApiRename);
  server.on("/api/delete", HTTP_POST, handleApiDelete);
  server.on("/api/unlock", HTTP_POST, handleApiUnlock);
  server.begin();
}

// -------------------------------------------------------------
// 8. MAIN LOOP
// -------------------------------------------------------------
void loop() {
  server.handleClient();

  if (isUnlocked && (millis() - unlockStartTime >= UNLOCK_DURATION)) {
    digitalWrite(LOCK_PIN, LOW);
    isUnlocked = false;
    addLog("Relocked (Engaged)", "warning");
  }

  // Instant Button Trigger (Debounced + Wait for Release)
  if (digitalRead(BUTTON_PIN) == LOW) {
    delay(40);
    if (digitalRead(BUTTON_PIN) == LOW) {
      while (digitalRead(BUTTON_PIN) == LOW) { delay(10); } // Wait for release
      runEnrollmentRoutine();
    }
  }

  if (!isUnlocked) {
    checkFingerprintAccess();
  }
}

// -------------------------------------------------------------
// 9. BIOMETRIC ACCESS CHECK
// -------------------------------------------------------------
void checkFingerprintAccess() {
  uint8_t p = finger.getImage();
  if (p != FINGERPRINT_OK) return;
  p = finger.image2Tz();
  if (p != FINGERPRINT_OK) return;
  p = finger.fingerFastSearch();

  if (p == FINGERPRINT_OK) {
    String userName = getUserName(finger.fingerID);
    triggerUnlock(userName + " (ID #" + String(finger.fingerID) + ")");
  } 
  else if (p == FINGERPRINT_NOTFOUND) {
    beepDenied();
    addLog("Denied: Unknown Print", "danger");
  }
}

// -------------------------------------------------------------
// 10. ADMIN ENROLLMENT
// -------------------------------------------------------------
void runEnrollmentRoutine() {
  beepAdminMode();
  
  // Find first vacant slot
  int nextID = -1;
  for (int id = 1; id <= 300; id++) {
    if (finger.loadModel(id) != FINGERPRINT_OK) {
      nextID = id;
      break;
    }
  }

  if (nextID == -1) { beepDenied(); return; }

  addLog("Enrolling: ID #" + String(nextID), "warning");

  int p = -1;
  unsigned long timeout = millis() + 10000;
  while (p != FINGERPRINT_OK) {
    p = finger.getImage();
    if (millis() > timeout) { beepDenied(); return; }
    delay(50);
  }
  if (finger.image2Tz(1) != FINGERPRINT_OK) { beepDenied(); return; }

  beepShort();
  delay(1500);
  while (finger.getImage() != FINGERPRINT_NOFINGER) { delay(50); }
  beepShort();

  p = -1;
  timeout = millis() + 10000;
  while (p != FINGERPRINT_OK) {
    p = finger.getImage();
    if (millis() > timeout) { beepDenied(); return; }
    delay(50);
  }
  if (finger.image2Tz(2) != FINGERPRINT_OK) { beepDenied(); return; }

  if (finger.createModel() != FINGERPRINT_OK) {
    beepDenied();
    return;
  }

  if (finger.storeModel(nextID) == FINGERPRINT_OK) {
    enrolledIDs.push_back(nextID); // Update cache immediately!
    addLog("Registered: ID #" + String(nextID), "success");
    beepSuccessLong();
  } else {
    beepDenied();
  }

  while (finger.getImage() != FINGERPRINT_NOFINGER) { delay(50); }
  delay(500);
}