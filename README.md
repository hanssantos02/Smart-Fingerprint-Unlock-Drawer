# SmartDrawer — IoT Biometric Smart Drawer Lock

An ESP32-based smart drawer lock with fingerprint authentication, buzzer feedback, auto-relock, and a mobile-first web dashboard for remote monitoring and user management.

Access the dashboard from any phone/laptop on the same Wi-Fi at `http://smartdrawer.local` or the ESP32's IP address.

## Features

- **Fingerprint unlock** — Fast search via optical fingerprint sensor (up to 300 IDs)
- **Web dashboard (mobile-first)** — Live lock status, user count, Wi-Fi signal, activity feed
- **Remote unlock** — One-tap unlock from the web UI
- **User management** — Rename and delete fingerprints from the browser, names saved in flash via `Preferences`
- **One-button enrollment** — Physical button starts a guided 2-scan enrollment into the first free ID slot
- **Auto-relock** — Lock re-engages automatically after 3 seconds
- **Activity log** — Last 10 events with NTP timestamps (unlock, denied, enroll, rename, delete, relock)
- **Audio feedback** — Different buzzer patterns for success, denied, enroll, and admin mode
- **Fast RAM cache** — Enrolled IDs are indexed once at boot so `/api/status` responds in ~1 ms
- **mDNS + NTP** — Reachable as `smartdrawer.local`, logs use real local time (GMT+8 by default)

## Hardware Required

| Component | Notes |
|---|---|
| ESP32 DevKit (30-pin) | Tested with Arduino core for ESP32 |
| Optical fingerprint sensor (e.g. R307 / AS608) | UART, 57600 baud |
| Solenoid lock / Electric latch + relay or MOSFET driver | Driven from `LOCK_PIN` |
| Active buzzer | `BUZZER_PIN` |
| Momentary push button | `BUTTON_PIN`, wired to GND (uses `INPUT_PULLUP`) |
| 5V / 3.3V power supply | Solenoid needs its own adequate supply, common GND with ESP32 |

## Wiring

| ESP32 GPIO | Connected To | Mode |
|---|---|---|
| GPIO 26 (RX2) | Fingerprint TX | `Serial2` 57600 8N1 |
| GPIO 27 (TX2) | Fingerprint RX | `Serial2` 57600 8N1 |
| GPIO 25 | Buzzer (+) / signal | `OUTPUT` |
| GPIO 32 | Lock relay / MOSFET gate | `OUTPUT`, HIGH = unlocked for 3 s |
| GPIO 14 | Enroll button → GND | `INPUT_PULLUP`, press to enroll |
| 3V3 / 5V / GND | Sensor VCC / GND | Check your sensor's voltage (usually 3.3V logic, 5V power OK) |

> Power the solenoid separately. Do not drive a lock directly from a GPIO pin — use a relay module or MOSFET.

## Software Required

- [Arduino IDE](https://www.arduino.cc/en/software) (or PlatformIO) with **ESP32 board package** installed
- Libraries (install via Library Manager):
  - `Adafruit Fingerprint Sensor Library` by Adafruit
  - Built-in to ESP32 core: `WiFi`, `WebServer`, `ESPmDNS`, `Preferences`

Project structure:

```text
SmartDrawer/
├── SmartDrawer.ino   # All firmware + embedded web UI
└── README.md
```

## Setup

1. Clone / copy this folder and open `SmartDrawer.ino` in Arduino IDE.
2. Select your board: `Tools > Board > ESP32 Arduino > ESP32 Dev Module`, then select the correct COM port.
3. Set your Wi-Fi credentials in the code before flashing:

```cpp
const char* ssid     = "YOUR_WIFI_SSID";
const char* password = "YOUR_WIFI_PASSWORD";
const char* hostName = "smartdrawer"; // -> http://smartdrawer.local
```

4. Adjust time zone if needed:

```cpp
const long gmtOffset_sec = 8 * 3600; // e.g. Philippines = UTC+8
const int  daylightOffset_sec = 0;
```

5. Upload to the ESP32, then open Serial Monitor at **115200 baud** to see:
   - `>> Indexing enrolled fingerprints... Done! Found N users.`
   - Wi-Fi connection dots, then `>> mDNS Started! Access at: http://smartdrawer.local`
6. Connect your phone/laptop to the same Wi-Fi and open `http://smartdrawer.local` (or the IP shown in Serial Monitor).

## Usage

### Unlock with fingerprint
1. Place an enrolled finger on the sensor.
2. Buzzer beeps once, lock energizes, dashboard shows `UNLOCKED`.
3. Lock auto-relocks after 3 seconds (`UNLOCK_DURATION`).

If the print is unknown: triple-beep + `Denied: Unknown Print` log entry.

### Enroll a new fingerprint
1. Press and release the button on GPIO 14.
2. You hear the admin double-beep, Serial / dashboard shows `Enrolling: ID #N`.
3. Place finger → remove when prompted (short beep) → place same finger again.
4. Long beep = success (`Registered: ID #N`). Triple-beep = failed / timeout (10 s per scan).
5. Rename the new `User #N` from the web dashboard.

Enrollment always uses the first free ID from 1–300.

### Web dashboard
Polls `/api/status` every 2 seconds:

- **Status pill:** `LOCKED` (red) / `UNLOCKED` (green)
- **Remote Web Unlock** button → `POST /api/unlock`
- **Metrics:** Registered Users count, Wi-Fi RSSI (dBm)
- **Registered User Profiles:** `ID #N` + editable name field + `Save` + delete (🗑️) buttons
- **Live Security Activity:** newest-first feed of the last 10 events

## API Reference

| Method | Endpoint | Params | Description |
|---|---|---|---|
| `GET` | `/` | — | Mobile-first HTML dashboard |
| `GET` | `/api/status` | — | JSON: `{ unlocked, users, rssi, userProfiles[{id,name}], logs[{time,msg,type}] }` |
| `POST` | `/api/unlock` | — | Remote unlock, logs `Unlocked: Web Remote` |
| `POST` | `/api/rename` | `?id=N&name=String` | Rename user, saved to `Preferences` namespace `user_names` |
| `POST` | `/api/delete` | `?id=N` | Delete fingerprint model + name, updates RAM cache |

Example:

```bash
curl http://smartdrawer.local/api/status
curl -X POST "http://smartdrawer.local/api/rename?id=1&name=Alice"
curl -X POST "http://smartdrawer.local/api/delete?id=2"
curl -X POST http://smartdrawer.local/api/unlock
```

## Buzzer & Log Behavior

| Event | Buzzer | Log entry |
|---|---|---|
| Fingerprint match | 150 ms beep | `Unlocked: <Name> (ID #N)` — success |
| Unknown finger | 3× 80 ms beeps | `Denied: Unknown Print` — danger |
| Web unlock | 150 ms beep | `Unlocked: Web Remote` — success |
| Auto-relock | — | `Relocked (Engaged)` — warning |
| Enroll start | 2× 100 ms beeps | `Enrolling: ID #N` — warning |
| Enroll success | 400 ms beep | `Registered: ID #N` — success |
| Rename | — | `Renamed: ID #N to <Name>` — warning |
| Delete | 80 ms beep | `Deleted: <Name> (ID #N)` — danger |
| Boot + Wi-Fi OK | 150 ms beep | `System Online (Wi-Fi)` — warning |

## Customization

All in the top of `SmartDrawer.ino`:

```cpp
#define BUZZER_PIN 25
#define LOCK_PIN   32
#define BUTTON_PIN 14

const unsigned long UNLOCK_DURATION = 3000; // ms lock stays open
const int MAX_LOGS = 10;                    // web activity feed length
```

- Change `UNLOCK_DURATION` for a longer open time.
- Change `MAX_LOGS` for a longer/shorter feed.
- `refreshEnrolledCache()` scans IDs 1–300 at boot — lower the upper bound for faster boot if you only need e.g. 50 users.

## Security Notes

- The web UI has **no login / auth**. Anyone on your LAN can unlock, rename, or delete users. Only use on a trusted network, or add HTTP auth / AP isolation.
- Fingerprint templates stay on the sensor module; only IDs (1–300) and display names (in ESP32 flash) are used by the firmware.

## Troubleshooting

- **Stuck at boot / no Serial output:** check `Serial2` wiring (ESP32 RX26 ↔ sensor TX, ESP32 TX27 ↔ sensor RX), sensor baud 57600, common GND.
- **`verifyPassword` fails / hangs:** wrong sensor wiring, wrong baud, or 5V sensor powered from weak 3.3V rail.
- **Can't open `smartdrawer.local`:** mDNS needs same subnet + mDNS support (Bonjour on Windows, Avahi on Linux). Use the IP from Serial Monitor as fallback.
- **`Time Syncing...` in logs:** NTP (`pool.ntp.org`) unreachable — check internet access; time fixes itself once online.
- **Slow dashboard:** normally caused by skipping `refreshEnrolledCache()` — this build already caches IDs in RAM, keep it.
- **Enrollment times out:** 10 s per scan, lift finger fully between scans, wipe sensor if oily.
