#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <Adafruit_Fingerprint.h>
#include <Preferences.h>
#include <vector>
#include "time.h"

const char* hostName = "smartdrawer";

const char* AP_SSID = "SmartDrawer-Setup";
const char* AP_PASS = "12345678"; // min 8 chars; leave "" for open AP
const byte DNS_PORT = 53;

const char* ntpServer = "pool.ntp.org";
const long  gmtOffset_sec = 8 * 3600;
const int   daylightOffset_sec = 0;

String wifiSSID = "";
String wifiPass = "";
bool apMode = false;
DNSServer dnsServer;

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

std::vector<int> enrolledIDs;

struct LogEntry {
  String timestamp;
  String message;
  String badgeClass;
};

const int MAX_LOGS = 10;
LogEntry accessLogs[MAX_LOGS];
int logCount = 0;

void checkFingerprintAccess();
void runEnrollmentRoutine();
void startConfigPortal();
void addLog(String message, String badgeClass);
void beepSuccess();
void beepAdminMode();
void beepDenied();

// -------------------------------------------------------------
// 3b. WI-FI CREDENTIAL STORAGE + CONNECTION
// -------------------------------------------------------------
void loadWiFiCredentials() {
  prefs.begin("wifi_cfg", true);
  wifiSSID = prefs.getString("ssid", "");
  wifiPass = prefs.getString("pass", "");
  prefs.end();
}

void saveWiFiCredentials(const String& ssid, const String& pass) {
  prefs.begin("wifi_cfg", false);
  prefs.putString("ssid", ssid);
  prefs.putString("pass", pass);
  prefs.end();
  wifiSSID = ssid;
  wifiPass = pass;
}

void clearWiFiCredentials() {
  prefs.begin("wifi_cfg", false);
  prefs.clear();
  prefs.end();
  wifiSSID = "";
  wifiPass = "";
}

// Returns true if connected within timeoutMs.
bool connectToWiFi(const String& ssid, const String& pass, unsigned long timeoutMs = 15000) {
  if (ssid.length() == 0) return false;
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(hostName);
  WiFi.begin(ssid.c_str(), pass.c_str());
  Serial.printf(">> Connecting to Wi-Fi \"%s\" ", ssid.c_str());
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - start) < timeoutMs) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  return WiFi.status() == WL_CONNECTED;
}

void onWiFiConnected() {
  apMode = false;
  Serial.print(">> Wi-Fi connected! IP: ");
  Serial.println(WiFi.localIP());
  if (MDNS.begin(hostName)) {
    Serial.println(">> mDNS Started! Access at: http://smartdrawer.local");
  }
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  addLog("System Online (Wi-Fi)", "warning");
  beepSuccess();
}

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
    *,*::before,*::after{box-sizing:border-box;margin:0;padding:0}
    :root{
      --ground:#0a0c10; --plate:#12161d; --well:#0b0f14; --line:#232b36; --line-soft:#1b222c;
      --ink:#e8edf2; --ink-2:#9aa6b2; --ink-3:#7c8894;
      --amber:#e8b44a; --amber-ink:#1a1405;
      --green:#3ecf7a; --red:#e5605c;
      --r:12px;
    }
    html{-webkit-text-size-adjust:100%}
    body{
      background:var(--ground); color:var(--ink);
      font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,"Helvetica Neue",Arial,sans-serif;
      padding:12px 10px 28px; display:flex; justify-content:center; overflow-x:hidden;
      font-variant-numeric:tabular-nums;
    }
    ::selection{background:var(--amber); color:var(--amber-ink)}
    :focus-visible{outline:2px solid var(--amber); outline-offset:2px; border-radius:6px}
    .wrap{width:100%; max-width:440px; display:flex; flex-direction:column; gap:10px}
    /* top instrument bar */
    .topbar{
      display:flex; align-items:center; justify-content:space-between; gap:10px;
      background:var(--plate); border:1px solid var(--line); border-radius:var(--r);
      padding:10px 12px;
    }
    .brand{display:flex; align-items:center; gap:10px; min-width:0}
    .mark{
      width:34px; height:34px; flex:0 0 34px; display:grid; place-items:center;
      background:var(--well); border:1px solid var(--line); border-radius:9px; color:var(--amber);
    }
    .mark svg{width:20px; height:20px}
    .brand-tx{display:flex; flex-direction:column; min-width:0}
    .brand-tx strong{font-size:.82rem; letter-spacing:.08em; font-weight:800}
    .brand-tx em{font-style:normal; font-size:.75rem; color:var(--ink-2); white-space:nowrap; overflow:hidden; text-overflow:ellipsis}
    .brand-tx em i{display:inline-block; width:7px; height:7px; border-radius:50%; background:var(--green); margin-right:5px; vertical-align:1px}
    .brand-tx em i.off{background:var(--red)}
    .link{display:flex; align-items:center; gap:8px}
    .bars{display:flex; align-items:flex-end; gap:2px; height:16px}
    .bars i{width:4px; background:var(--line); border-radius:2px; display:block}
    .bars i:nth-child(1){height:5px}.bars i:nth-child(2){height:9px}.bars i:nth-child(3){height:12px}.bars i:nth-child(4){height:16px}
    .bars i.on{background:var(--amber)}
    .dbm{font-size:.75rem; color:var(--ink-2); min-width:62px; text-align:right}
    /* lock deck */
    .deck{background:var(--plate); border:1px solid var(--line); border-radius:14px; padding:14px 14px 12px}
    .deck-head{display:flex; align-items:baseline; justify-content:space-between; gap:10px}
    #lockStatus{font-size:1.45rem; font-weight:800; letter-spacing:.04em}
    #lockStatus.locked{color:var(--red)} #lockStatus.open{color:var(--green)}
    .live{font-size:.75rem; color:var(--ink-2); display:flex; align-items:center; gap:6px; white-space:nowrap}
    .live i{width:7px; height:7px; border-radius:50%; background:var(--green); animation:pulse 2s infinite}
    @keyframes pulse{0%,100%{opacity:1}50%{opacity:.35}}
    @media (prefers-reduced-motion:reduce){.live i{animation:none}.bolt-block{transition:none}}
    .bolt{margin-top:10px; background:var(--well); border:1px solid var(--line-soft); border-radius:10px; padding:12px}
    .slot{position:relative; height:38px; background:#07090c; border:1px solid var(--line-soft); border-radius:8px; overflow:hidden}
    .slot::after{content:""; position:absolute; inset:0; background-image:repeating-linear-gradient(90deg,transparent 0 18px,rgba(255,255,255,.045) 18px 19px); pointer-events:none}
    .bolt-block{
      position:absolute; top:4px; bottom:4px; left:4px; width:calc(50% - 8px);
      background:#2a323d; border:1px solid #3a4451; border-radius:6px;
      transition:transform .22s ease-out;
    }
    .bolt-block::before,.bolt-block::after{content:""; position:absolute; top:6px; bottom:6px; width:1px; background:rgba(255,255,255,.14)}
    .bolt-block::before{left:10px}.bolt-block::after{left:15px}
    .bolt.open .bolt-block{transform:translateX(100%); background:#2e3a2f; border-color:#3f5a48}
    .bolt-scale{display:flex; justify-content:space-between; margin-top:7px; font-size:.75rem; letter-spacing:.1em; color:var(--ink-3); font-weight:700}
    .bolt.locked .bolt-scale span:first-child{color:var(--red)}
    .bolt.open .bolt-scale span:last-child{color:var(--green)}
    .relock{height:3px; background:#07090c; border-radius:2px; margin-top:8px; overflow:hidden}
    .relock i{display:block; height:100%; width:0; background:var(--green)}
    .relock i.run{animation:drain 3s linear forwards}
    @keyframes drain{from{width:100%}to{width:0%}}
    .unlock{
      width:100%; margin-top:12px; min-height:52px; border:none; border-radius:10px;
      background:var(--amber); color:var(--amber-ink);
      font-size:1rem; font-weight:800; letter-spacing:.01em; cursor:pointer;
    }
    .unlock:hover{background:#f0c15e}
    .unlock:active{transform:translateY(1px)}
    .unlock:disabled{opacity:.55; cursor:wait; transform:none}
    .last{margin-top:8px; font-size:.78rem; color:var(--ink-2); white-space:nowrap; overflow:hidden; text-overflow:ellipsis}
    .readouts{display:flex; margin-top:10px; background:var(--well); border:1px solid var(--line-soft); border-radius:10px; overflow:hidden}
    .readouts div{flex:1; padding:9px 6px; text-align:center}
    .readouts div+div{border-left:1px solid var(--line-soft)}
    .readouts b{display:block; font-size:.95rem}
    .readouts span{font-size:.75rem; letter-spacing:.1em; text-transform:uppercase; color:var(--ink-3); font-weight:700}
    /* panels */
    .panel{background:var(--plate); border:1px solid var(--line); border-radius:var(--r); padding:14px}
    .panel-head{display:flex; align-items:baseline; justify-content:space-between; gap:8px; margin-bottom:4px}
    .panel-head h2{font-size:.92rem; font-weight:800; letter-spacing:.02em}
    .panel-head span{font-size:.75rem; color:var(--ink-3)}
    .hint{font-size:.78rem; color:var(--ink-2); margin:2px 0 10px; line-height:1.5}
    .hint code{font-family:ui-monospace,SFMono-Regular,Menlo,Consolas,monospace; font-size:.7rem; color:var(--amber); background:var(--well); border:1px solid var(--line-soft); padding:1px 5px; border-radius:5px}
    .urow{border-top:1px solid var(--line-soft); padding:10px 0; display:grid; grid-template-columns:30px 1fr auto; gap:10px; align-items:center}
    .urow:first-of-type{border-top:none; padding-top:2px}
    .fp{width:30px; height:30px; display:grid; place-items:center; background:var(--well); border:1px solid var(--line-soft); border-radius:8px; color:var(--ink-2)}
    .fp svg{width:17px; height:17px}
    .umain{min-width:0}
    .uid{font-family:ui-monospace,SFMono-Regular,Menlo,Consolas,monospace; font-size:.75rem; color:var(--ink-3); letter-spacing:.06em}
    .uname{width:100%; margin-top:3px; background:var(--well); border:1px solid #2a333f; color:var(--ink); padding:8px 9px; border-radius:8px; font-size:.86rem; outline:none; min-width:0}
    .uname:focus{border-color:var(--amber)}
    .uact{display:flex; gap:6px}
    .tbtn{border:1px solid var(--line); background:transparent; color:var(--ink); padding:8px 10px; border-radius:8px; font-size:.74rem; font-weight:700; cursor:pointer; min-height:36px}
    .tbtn:hover{border-color:var(--amber); color:var(--amber)}
    .tbtn.save{background:var(--amber); border-color:var(--amber); color:var(--amber-ink); display:none}
    .tbtn.save.show{display:block}
    .tbtn.danger{color:var(--red); border-color:#3a2a2b; padding:8px 9px}
    .tbtn.danger:hover{border-color:var(--red)}
    .confirm{grid-column:1/-1; display:flex; align-items:center; justify-content:space-between; gap:8px; background:#170f10; border:1px solid #3a2a2b; border-radius:8px; padding:8px 10px; font-size:.76rem}
    .confirm b{color:var(--red)}
    .confirm div{display:flex; gap:6px}
    .cbtn{border:none; border-radius:7px; padding:7px 11px; font-size:.74rem; font-weight:800; cursor:pointer}
    .cbtn.yes{background:var(--red); color:#fff}
    .cbtn.no{background:transparent; color:var(--ink-2); border:1px solid var(--line)}
    .empty{border:1px dashed var(--line); border-radius:10px; padding:16px 12px; text-align:center; color:var(--ink-2); font-size:.78rem; line-height:1.5}
    /* activity */
    .arow{display:flex; align-items:flex-start; gap:9px; padding:9px 0; border-top:1px solid var(--line-soft); font-size:.79rem}
    .arow:first-of-type{border-top:none}
    .dot{width:8px; height:8px; border-radius:50%; margin-top:5px; flex:0 0 8px}
    .dot.success{background:var(--green)} .dot.danger{background:var(--red)} .dot.warning{background:var(--amber)}
    .amsg{flex:1; min-width:0; overflow-wrap:anywhere; line-height:1.4}
    .atime{color:var(--ink-3); font-size:.75rem; white-space:nowrap; font-family:ui-monospace,SFMono-Regular,Menlo,Consolas,monospace}
    footer{font-size:.75rem; color:var(--ink-3); text-align:center; line-height:1.6; padding:2px 8px}
    #toast{
      position:fixed; left:50%; bottom:18px; transform:translateX(-50%) translateY(8px);
      background:#1c232d; border:1px solid var(--line); color:var(--ink);
      font-size:.78rem; font-weight:600; padding:10px 14px; border-radius:10px;
      opacity:0; pointer-events:none; transition:opacity .18s ease-out, transform .18s ease-out;
      max-width:min(420px,calc(100vw - 24px)); text-align:center;
    }
    #toast.show{opacity:1; transform:translateX(-50%) translateY(0)}
  </style>
</head>
<body>
  <div class="wrap">
    <header class="topbar">
      <div class="brand">
        <span class="mark" aria-hidden="true">
          <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.7" stroke-linecap="round" stroke-linejoin="round"><path d="M12 11c1.7 0 3 1.6 3 3.5 0 2.5-.5 5-1.5 7"/><path d="M9 14c0 2.5-.6 4.6-1.7 6.4"/><path d="M12 8c2.8 0 5 2.5 5 5.5 0 1.9-.2 3.7-.7 5.5"/><path d="M7.2 12.4c.3-2.9 2.4-5.4 4.8-5.4 3.9 0 7 3.4 7 7.5 0 1.5-.1 3-.4 4.5"/><path d="M4.9 13.5C5.4 9.7 8.4 6 12 6c4.4 0 8 3.8 8 8.5 0 2-.2 4-.6 5.5"/></svg>
        </span>
        <span class="brand-tx"><strong>SMARTDRAWER</strong><em><i id="netDot"></i><span id="hostLine">smartdrawer.local &middot; connecting</span></em></span>
      </div>
      <div class="link" title="Wi-Fi signal">
        <span class="bars" id="sigBars" aria-hidden="true"><i></i><i></i><i></i><i></i></span>
        <span class="dbm" id="wifiRSSI">-- dBm</span>
      </div>
    </header>

    <section class="deck" aria-label="Lock control">
      <div class="deck-head">
        <span id="lockStatus" class="locked">LOCKED</span>
        <span class="live"><i></i>live &middot; <span id="clock">--</span></span>
      </div>
      <div class="bolt locked" id="bolt">
        <div class="slot"><div class="bolt-block"></div></div>
        <div class="bolt-scale"><span>ENGAGED</span><span>DRAWN</span></div>
        <div class="relock"><i id="relockBar"></i></div>
      </div>
      <button class="unlock" id="unlockBtn" onclick="remoteUnlock()">Unlock drawer</button>
      <p class="last" id="lastEvent">Waiting for first poll&hellip;</p>
      <div class="readouts">
        <div><b id="userCount">--</b><span>prints</span></div>
        <div><b id="relockState">--</b><span>bolt</span></div>
        <div><b id="linkQ">--</b><span>link</span></div>
      </div>
    </section>

    <section class="panel" aria-label="Fingerprints">
      <div class="panel-head"><h2>Fingerprints</h2><span id="userTotal"></span></div>
      <p class="hint">To enroll a new print, press the <code>ENROLL</code> button on the drawer, place the finger, lift, then place it again.</p>
      <div id="userList"><div class="empty">Loading prints&hellip;</div></div>
    </section>

    <section class="panel" aria-label="Activity">
      <div class="panel-head"><h2>Activity</h2><span>last 10 &middot; polls every 2s</span></div>
      <div id="logBody"><div class="empty">No activity logged yet.</div></div>
    </section>

    <section class="panel" aria-label="Wi-Fi settings">
      <div class="panel-head"><h2>Wi-Fi</h2><span id="wifiIp"></span></div>
      <p class="hint">Connected to <code id="wifiSsid">--</code>. To move the drawer to another home / hotspot, enter the new details here — it reboots and connects automatically.</p>
      <label style="font-size:.75rem;color:var(--ink-2);display:block;margin:0 0 4px" for="newSsid">Wi-Fi name (SSID)</label>
      <input class="uname" id="newSsid" maxlength="32" autocomplete="off" placeholder="e.g. HomeWiFi" style="margin-top:0">
      <label style="font-size:.75rem;color:var(--ink-2);display:block;margin:10px 0 4px" for="newPass">Wi-Fi password</label>
      <input class="uname" id="newPass" type="password" maxlength="64" autocomplete="off" placeholder="Leave blank for open network" style="margin-top:0">
      <div style="display:flex;gap:8px;margin-top:12px">
        <button class="tbtn save show" style="flex:1" onclick="saveWifi()">Save &amp; reconnect</button>
        <button class="tbtn danger" onclick="askWifiReset()">Forget</button>
      </div>
      <div class="confirm" id="wifiCf" style="display:none;margin-top:10px"></div>
    </section>

    <footer>Local console &middot; polls <code>/api/status</code> every 2s &middot; web unlocks log as <code>Web Remote</code></footer>
  </div>
  <div id="toast" role="status"></div>

  <script>
    let editingId = null, pollOk = false, toastT = null, lastUnlockSeen = false;

    function esc(s){ return String(s == null ? "" : s).replace(/[&<>"']/g, function(c){ return {"&":"&amp;","<":"&lt;",">":"&gt;",'"':"&quot;","'":"&#39;"}[c]; }); }
    function toast(msg){
      const t = document.getElementById('toast');
      t.textContent = msg; t.classList.add('show');
      clearTimeout(toastT); toastT = setTimeout(function(){ t.classList.remove('show'); }, 2200);
    }
    function sigGrade(r){
      if (r === 0 || r == null) return { n: 0, q: "--" };
      if (r >= -55) return { n: 4, q: "strong" };
      if (r >= -67) return { n: 3, q: "good" };
      if (r >= -78) return { n: 2, q: "fair" };
      return { n: 1, q: "weak" };
    }
    function paintSignal(rssi){
      document.getElementById('wifiRSSI').textContent = (rssi === 0 ? "--" : rssi + " dBm");
      const g = sigGrade(rssi);
      const bars = document.getElementById('sigBars').children;
      for (let i = 0; i < bars.length; i++) bars[i].className = i < g.n ? "on" : "";
      document.getElementById('linkQ').textContent = g.q;
    }
    function paintBolt(open){
      const st = document.getElementById('lockStatus');
      const bolt = document.getElementById('bolt');
      const bar = document.getElementById('relockBar');
      st.textContent = open ? "UNLOCKED" : "LOCKED";
      st.className = open ? "open" : "locked";
      bolt.className = "bolt " + (open ? "open" : "locked");
      document.getElementById('relockState').textContent = open ? "drawn" : "engaged";
      if (open && !lastUnlockSeen) {
        bar.classList.remove('run'); void bar.offsetWidth; bar.classList.add('run');
      }
      if (!open) bar.classList.remove('run');
      lastUnlockSeen = open;
      const btn = document.getElementById('unlockBtn');
      btn.disabled = !!open;
      btn.textContent = open ? "Drawer open \u2014 relocks automatically" : "Unlock drawer";
    }
    function renderUsers(profiles){
      const box = document.getElementById('userList');
      if (!profiles.length) {
        box.innerHTML = "<div class='empty'>No prints enrolled yet.<br>Press the button on the drawer, then place the same finger twice.</div>";
        return;
      }
      let h = "";
      profiles.forEach(function(u){
        const dirty = editingId === u.id;
        h += "<div class='urow' data-id='" + u.id + "'>"
          + "<span class='fp'><svg viewBox='0 0 24 24' fill='none' stroke='currentColor' stroke-width='1.7' stroke-linecap='round'><path d='M12 11c1.7 0 3 1.6 3 3.5 0 2.5-.5 5-1.5 7'/><path d='M9 14c0 2.5-.6 4.6-1.7 6.4'/><path d='M12 8c2.8 0 5 2.5 5 5.5 0 1.9-.2 3.7-.7 5.5'/><path d='M7.2 12.4c.3-2.9 2.4-5.4 4.8-5.4 3.9 0 7 3.4 7 7.5 0 1.5-.1 3-.4 4.5'/></svg></span>"
          + "<div class='umain'><div class='uid'>ID #" + u.id + "</div>"
          + "<input class='uname' id='nameInput_" + u.id + "' value=\"" + esc(u.name) + "\" maxlength='24' autocomplete='off' spellcheck='false' onfocus='editingId=" + u.id + "' oninput='markDirty(" + u.id + ")'></div>"
          + "<div class='uact'><button class='tbtn save" + (dirty ? " show" : "") + "' id='save_" + u.id + "' onclick='saveName(" + u.id + ")'>Save</button>"
          + "<button class='tbtn danger' onclick='askDelete(" + u.id + ")' aria-label='Delete ID " + u.id + "'><svg width='14' height='14' viewBox='0 0 24 24' fill='none' stroke='currentColor' stroke-width='1.8' stroke-linecap='round'><path d='M4 7h16'/><path d='M9 7V5a1 1 0 0 1 1-1h4a1 1 0 0 1 1 1v2'/><path d='M6 7l1 13a1 1 0 0 0 1 1h8a1 1 0 0 0 1-1l1-13'/></svg></button></div>"
          + "<div class='confirm' id='cf_" + u.id + "' style='display:none'></div>"
          + "</div>";
      });
      const active = document.activeElement;
      const aid = active && active.id ? active.id : null;
      const selS = active && active.selectionStart != null ? active.selectionStart : null;
      box.innerHTML = h;
      if (aid && document.getElementById(aid)) {
        const el = document.getElementById(aid);
        el.focus({ preventScroll: true });
        try { if (selS != null) el.setSelectionRange(selS, selS); } catch (e) {}
      }
    }
    function markDirty(id){
      editingId = id;
      const b = document.getElementById('save_' + id);
      if (b) b.classList.add('show');
    }
    function renderLogs(logs){
      const box = document.getElementById('logBody');
      if (!logs.length) { box.innerHTML = "<div class='empty'>No activity logged yet.</div>"; return; }
      let h = "";
      logs.forEach(function(l){
        const t = (l.type === "success" || l.type === "danger" || l.type === "warning") ? l.type : "warning";
        h += "<div class='arow'><span class='dot " + t + "'></span><span class='amsg'>" + esc(l.msg) + "</span><span class='atime'>" + esc(l.time) + "</span></div>";
      });
      box.innerHTML = h;
      const last = document.getElementById('lastEvent');
      if (logs[0]) last.textContent = "Last: " + logs[0].msg + " \u00b7 " + logs[0].time;
    }
    function tickClock(){
      try {
        document.getElementById('clock').textContent = new Date().toLocaleTimeString([], { hour: 'numeric', minute: '2-digit', second: '2-digit' });
      } catch (e) {}
    }
    function updateDashboard() {
      fetch('/api/status', { cache: 'no-store' })
        .then(function(res){ if (!res.ok) throw 0; return res.json(); })
        .then(function(data){
          pollOk = true;
          document.getElementById('netDot').className = "";
          document.getElementById('hostLine').textContent = "smartdrawer.local \u00b7 live";
          paintBolt(!!data.unlocked);
          document.getElementById('userCount').textContent = data.users;
          document.getElementById('userTotal').textContent = data.users + (data.users === 1 ? " print" : " prints");
          paintSignal(data.rssi);
          if (data.ssid) document.getElementById('wifiSsid').textContent = data.ssid;
          if (data.ip) document.getElementById('wifiIp').textContent = data.ip;
          const ids = {};
          (data.userProfiles || []).forEach(function(u){ ids[u.id] = true; });
          if (editingId != null && !ids[editingId]) editingId = null;
          const active = document.activeElement;
          const typing = active && active.className === "uname";
          if (!typing) renderUsers(data.userProfiles || []);
          else {
            document.getElementById('userCount').textContent = data.users;
          }
          renderLogs(data.logs || []);
        })
        .catch(function(){
          pollOk = false;
          document.getElementById('netDot').className = "off";
          document.getElementById('hostLine').textContent = "smartdrawer.local \u00b7 retrying";
        });
    }
    function saveName(id) {
      const input = document.getElementById('nameInput_' + id);
      const v = input.value.trim().slice(0, 24);
      if (!v) { toast("Name can't be empty"); input.focus(); return; }
      input.blur();
      fetch('/api/rename?id=' + id + '&name=' + encodeURIComponent(v), { method: 'POST' })
        .then(function(r){ if (!r.ok) throw 0; editingId = null; toast("Saved ID #" + id + " as " + v); updateDashboard(); })
        .catch(function(){ toast("Rename failed \u2014 drawer unreachable"); });
    }
    function askDelete(id){
      const inp = document.getElementById('nameInput_' + id);
      const name = inp ? inp.value.trim() : ("ID #" + id);
      const cf = document.getElementById('cf_' + id);
      cf.style.display = "flex";
      cf.innerHTML = "<span>Delete <b>" + esc(name) + " (#" + id + ")?</b></span><div><button class='cbtn yes' onclick='deleteUser(" + id + ")'>Delete</button><button class='cbtn no' onclick='hideDelete(" + id + ")'>Keep</button></div>";
    }
    function hideDelete(id){
      const cf = document.getElementById('cf_' + id);
      cf.style.display = "none"; cf.innerHTML = "";
    }
    function deleteUser(id) {
      fetch('/api/delete?id=' + id, { method: 'POST' })
        .then(function(r){ if (!r.ok) throw 0; if (editingId === id) editingId = null; toast("Deleted ID #" + id); updateDashboard(); })
        .catch(function(){ toast("Delete failed \u2014 drawer unreachable"); });
    }
    function remoteUnlock() {
      const b = document.getElementById('unlockBtn');
      b.disabled = true; b.textContent = "Sending\u2026";
      fetch('/api/unlock', { method: 'POST' })
        .then(function(r){ if (!r.ok) throw 0; toast("Unlock signal sent"); updateDashboard(); })
        .catch(function(){ toast("Unlock failed \u2014 drawer unreachable"); b.disabled = false; b.textContent = "Unlock drawer"; });
    }
    function saveWifi() {
      const s = document.getElementById('newSsid').value.trim();
      const p = document.getElementById('newPass').value;
      if (!s) { toast("Enter a Wi-Fi name"); return; }
      if (!confirm("Connect drawer to \"" + s + "\"? It will reboot.")) return;
      toast("Saving \u2014 drawer is rebooting\u2026");
      fetch('/api/wifisave?ssid=' + encodeURIComponent(s) + '&pass=' + encodeURIComponent(p), { method: 'POST' })
        .then(function(){ toast("Saved! Rejoin " + s + " after reboot"); })
        .catch(function(){ toast("Save sent \u2014 drawer is rebooting"); });
    }
    function askWifiReset(){
      const cf = document.getElementById('wifiCf');
      cf.style.display = "flex";
      cf.innerHTML = "<span>Forget Wi-Fi and reboot into <b>setup mode</b>?</span><div><button class='cbtn yes' onclick='wifiReset()'>Forget</button><button class='cbtn no' onclick=\"document.getElementById('wifiCf').style.display='none'\">Keep</button></div>";
    }
    function wifiReset(){
      fetch('/api/wifireset', { method: 'POST' })
        .then(function(){ toast("Forgetting Wi-Fi \u2014 join SmartDrawer-Setup"); document.getElementById('wifiCf').style.display='none'; })
        .catch(function(){ toast("Reset sent \u2014 join SmartDrawer-Setup"); });
    }
    document.addEventListener('keydown', function(e){
      if (e.key === "Enter" && document.activeElement && document.activeElement.className === "uname") {
        const id = parseInt(document.activeElement.id.replace("nameInput_", ""), 10);
        if (id) saveName(id);
      }
      if (e.key === "Escape" && document.activeElement && document.activeElement.blur) document.activeElement.blur();
    });
    document.addEventListener('focusout', function(e){
      if (e.target && e.target.className === "uname") setTimeout(function(){ if (!document.activeElement || document.activeElement.className !== "uname") editingId = null; }, 250);
    });
    tickClock(); setInterval(tickClock, 1000);
    setInterval(updateDashboard, 2000);
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
  json += "\"rssi\":" + String(apMode ? 0 : WiFi.RSSI()) + ",";
  json += "\"ssid\":\"" + (apMode ? String(AP_SSID) + " (setup mode)" : WiFi.SSID()) + "\",";
  json += "\"ip\":\"" + (apMode ? WiFi.softAPIP().toString() : WiFi.localIP().toString()) + "\",";
  json += "\"apMode\":" + String(apMode ? "true" : "false") + ",";
  
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
// 6b. WI-FI SETUP PORTAL + MANAGEMENT APIs
// -------------------------------------------------------------
String wifiPortalHtml(const String& msg = "", bool isError = false, bool doScan = true) {
  String options = "";
  if (doScan && apMode) {
    int n = WiFi.scanNetworks();
    for (int i = 0; i < n; i++) {
      int rssi = WiFi.RSSI(i);
      String sig = (rssi >= -67) ? "good" : (rssi >= -78 ? "fair" : "weak");
      options += "<li><button type=\"button\" class=\"net\" onclick=\"pick('" +
                 WiFi.SSID(i) + "')\"><b>" + WiFi.SSID(i) + "</b><span>" +
                 String(rssi) + " dBm &middot; " + sig + "</span></button></li>";
    }
  }
  if (options.length() == 0) {
    options = "<li class=\"empty\">No networks found yet — <a href=\"/\">rescan</a> or type the name manually.</li>";
  }
  String banner = "";
  if (msg.length()) {
    banner = "<p class=\"" + String(isError ? "err" : "ok") + "\">" + msg + "</p>";
  }
  String html = "<!DOCTYPE html><html><head><meta charset=\"UTF-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>SmartDrawer Wi-Fi Setup</title>"
    "<style>body{font-family:-apple-system,Segoe UI,Roboto,sans-serif;background:#0a0c10;color:#e8edf2;margin:0;padding:16px;display:flex;justify-content:center}"
    ".c{width:100%;max-width:420px;background:#12161d;border:1px solid #232b36;border-radius:12px;padding:18px}"
    "h1{font-size:1.1rem;margin:0 0 4px}p.sub{color:#9aa6b2;font-size:.82rem;margin:0 0 12px;line-height:1.5}"
    "label{font-size:.75rem;color:#9aa6b2;display:block;margin:10px 0 4px}input{width:100%;box-sizing:border-box;background:#0b0f14;border:1px solid #2a333f;color:#fff;padding:11px;border-radius:8px;font-size:.9rem}"
    "button.go{width:100%;margin-top:14px;background:#e8b44a;border:none;border-radius:10px;padding:13px;font-weight:800;font-size:.95rem;cursor:pointer;color:#1a1405}"
    "ul{list-style:none;padding:0;margin:12px 0 0}li{margin-bottom:8px}.net{width:100%;display:flex;justify-content:space-between;align-items:center;background:#0b0f14;border:1px solid #232b36;color:#e8edf2;border-radius:8px;padding:10px;cursor:pointer;font-size:.85rem}.net span{color:#7c8894;font-size:.75rem}"
    ".empty{color:#9aa6b2;font-size:.8rem;border:1px dashed #232b36;border-radius:8px;padding:12px;text-align:center}a{color:#e8b44a}"
    ".ok{background:#10231a;border:1px solid #3f5a48;color:#3ecf7a;border-radius:8px;padding:10px;font-size:.8rem}"
    ".err{background:#2a1214;border:1px solid #5a2f30;color:#e5605c;border-radius:8px;padding:10px;font-size:.8rem}</style></head><body>"
    "<div class=\"c\"><h1>SmartDrawer Wi-Fi Setup</h1>"
    "<p class=\"sub\">Connect your drawer to Wi-Fi. Join <b>" + String(AP_SSID) + "</b>, open this page, pick your home network, then reboot — no code changes needed.</p>"
    + banner +
    "<form method=\"POST\" action=\"/wifisave\">"
    "<label>Wi-Fi name (SSID)</label><input id=\"ssid\" name=\"ssid\" maxlength=\"32\" required placeholder=\"e.g. HomeWiFi\">"
    "<label>Wi-Fi password</label><input name=\"pass\" type=\"password\" maxlength=\"64\" placeholder=\"Leave blank for open network\">"
    "<button class=\"go\" type=\"submit\">Save &amp; Connect</button></form>"
    "<ul>" + options + "</ul>"
    "<script>function pick(s){document.getElementById('ssid').value=s;window.scrollTo(0,0);}</script>"
    "</div></body></html>";
  return html;
}

void handleWifiPortalRoot() {
  server.send(200, "text/html", wifiPortalHtml());
}

void handleWifiScan() {
  int n = WiFi.scanNetworks();
  String json = "[";
  for (int i = 0; i < n; i++) {
    if (i > 0) json += ",";
    json += "{\"ssid\":\"" + WiFi.SSID(i) + "\",\"rssi\":" + String(WiFi.RSSI(i)) + "}";
  }
  json += "]";
  server.send(200, "application/json", json);
}

// Used by BOTH the captive portal (/wifisave) and dashboard (/api/wifisave).
void saveWifiAndReboot(const String& ssid, const String& pass) {
  saveWiFiCredentials(ssid, pass);
  Serial.printf(">> New Wi-Fi saved: \"%s\" — rebooting...\n", ssid.c_str());
  addLog("Wi-Fi updated: " + ssid + " (rebooting)", "warning");
  server.send(200, "text/html",
    "<!DOCTYPE html><html><body style=\"font-family:sans-serif;background:#0a0c10;color:#fff;text-align:center;padding:40px\">"
    "<h2>Saved! Connecting to " + ssid + "...</h2>"
    "<p>Drawer is rebooting. Rejoin your home Wi-Fi, then open <b>http://smartdrawer.local</b></p>"
    "</body></html>");
  delay(1000);
  ESP.restart();
}

void handleWifiSave() {
  if (!server.hasArg("ssid")) {
    server.send(400, "text/html", wifiPortalHtml("Missing Wi-Fi name.", true));
    return;
  }
  String ssid = server.arg("ssid");
  ssid.trim();
  String pass = server.hasArg("pass") ? server.arg("pass") : String("");
  if (ssid.length() == 0 || ssid.length() > 32) {
    server.send(400, "text/html", wifiPortalHtml("Wi-Fi name is empty or too long.", true));
    return;
  }
  saveWifiAndReboot(ssid, pass);
}

void handleApiWifi() {
  String json = "{\"ssid\":\"" + (apMode ? "" : WiFi.SSID()) + "\",";
  json += "\"ip\":\"" + (apMode ? WiFi.softAPIP().toString() : WiFi.localIP().toString()) + "\",";
  json += "\"apMode\":" + String(apMode ? "true" : "false") + "}";
  server.send(200, "application/json", json);
}

void handleApiWifiSave() {
  handleWifiSave(); // same validation + reboot flow
}

void handleApiWifiReset() {
  clearWiFiCredentials();
  Serial.println(">> Wi-Fi credentials erased — rebooting into setup mode.");
  server.send(200, "text/plain", "OK - rebooting into setup mode");
  delay(1000);
  ESP.restart();
}

void startConfigPortal() {
  apMode = true;
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(AP_SSID, AP_PASS);
  delay(500);
  IPAddress apIP = WiFi.softAPIP();
  Serial.print(">> Setup mode! Join Wi-Fi \"");
  Serial.print(AP_SSID);
  Serial.print("\" then open http://");
  Serial.println(apIP);
  dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
  dnsServer.start(DNS_PORT, "*", apIP); // captive-portal: all domains -> drawer
  addLog("Setup Mode: join " + String(AP_SSID), "warning");
  beepAdminMode();
}

void handleNotFound() {
  if (apMode) {
    // Captive-portal: phones probe random URLs — return setup page fast (no rescan).
    server.send(200, "text/html", wifiPortalHtml("", false, false));
  } else {
    server.send(404, "text/plain", "Not Found");
  }
}

void registerWebRoutes() {
  // In setup (AP) mode the root page IS the Wi-Fi portal so new owners
  // land straight on the form; once connected it serves the dashboard.
  if (apMode) {
    server.on("/", HTTP_GET, handleWifiPortalRoot);
  } else {
    server.on("/", HTTP_GET, handleRoot);
  }
  server.on("/api/status", HTTP_GET, handleApiStatus);
  server.on("/api/rename", HTTP_POST, handleApiRename);
  server.on("/api/delete", HTTP_POST, handleApiDelete);
  server.on("/api/unlock", HTTP_POST, handleApiUnlock);
  // Wi-Fi management (dashboard, same-LAN)
  server.on("/api/wifi", HTTP_GET, handleApiWifi);
  server.on("/api/wifisave", HTTP_POST, handleApiWifiSave);
  server.on("/api/wifireset", HTTP_POST, handleApiWifiReset);
  server.on("/api/scan", HTTP_GET, handleWifiScan);
  // Captive-portal pages (setup AP mode)
  server.on("/wifisave", HTTP_POST, handleWifiSave);
  server.onNotFound(handleNotFound);
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

  // --- Wi-Fi: try saved credentials, else start setup portal ---
  loadWiFiCredentials();
  bool connected = connectToWiFi(wifiSSID, wifiPass, 15000);
  if (connected) {
    onWiFiConnected();
  } else {
    if (wifiSSID.length() > 0) {
      Serial.println(">> Saved Wi-Fi failed. Starting setup portal (old credentials kept).");
    } else {
      Serial.println(">> No Wi-Fi saved. Starting setup portal.");
    }
    startConfigPortal();
  }

  registerWebRoutes();
  server.begin();
  if (apMode) {
    Serial.println(">> HTTP server in SETUP mode. Configure at http://192.168.4.1");
  }
}

// -------------------------------------------------------------
// 8. MAIN LOOP
// -------------------------------------------------------------
void loop() {
  if (apMode) dnsServer.processNextRequest();
  server.handleClient();

  if (isUnlocked && (millis() - unlockStartTime >= UNLOCK_DURATION)) {
    digitalWrite(LOCK_PIN, LOW);
    isUnlocked = false;
    addLog("Relocked (Engaged)", "warning");
  }

  // Button: short press = enroll, 5s long-press = forget Wi-Fi + setup mode.
  if (digitalRead(BUTTON_PIN) == LOW) {
    delay(40);
    if (digitalRead(BUTTON_PIN) == LOW) {
      unsigned long pressStart = millis();
      while (digitalRead(BUTTON_PIN) == LOW) {
        // Long-press feedback: after 5s, beep + reboot into setup mode.
        if (millis() - pressStart >= 5000) {
          beepDenied();
          Serial.println(">> Long-press: erasing Wi-Fi, rebooting into setup mode.");
          addLog("Wi-Fi reset (button)", "danger");
          clearWiFiCredentials();
          delay(500);
          ESP.restart();
          return;
        }
        delay(10);
      }
      runEnrollmentRoutine();
    }
  }

  if (!isUnlocked) {
    // Fingerprint works in both normal and setup mode so the owner is never locked out.
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