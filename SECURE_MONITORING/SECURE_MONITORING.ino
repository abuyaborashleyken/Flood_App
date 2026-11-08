// AquaGuard ESP32 - ultrasonic-only flood-distance monitor.
// LED and alarm status uses the actual distance (cm) to the water surface.
// Distance DECREASES as the water rises toward the sensor.
// SAFE: 11 cm and farther; WARNING: 7 cm to less than 11 cm;
// CRITICAL: less than 7 cm (including 6-0 cm if measurable).


#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <esp_arduino_version.h>
#include <stdlib.h>
#include <time.h>

#if __has_include("secrets.h")
#include "secrets.h"
#else
#define AQUAGUARD_AP_PASSWORD "change-me-before-use"
#endif

#ifndef AQUAGUARD_WIFI_SSID
#define AQUAGUARD_WIFI_SSID ""
#endif
#ifndef AQUAGUARD_WIFI_PASSWORD
#define AQUAGUARD_WIFI_PASSWORD ""
#endif
#ifndef AQUAGUARD_DEVICE_TOKEN
#define AQUAGUARD_DEVICE_TOKEN "configure-a-private-device-token"
#endif
#ifndef AQUAGUARD_TELEMETRY_URL
#define AQUAGUARD_TELEMETRY_URL "https://secure-app-live.vercel.app/api/telemetry"
#endif

constexpr uint8_t RED_LED_PIN = 2;
constexpr uint8_t YELLOW_LED_PIN = 22;
constexpr uint8_t GREEN_LED_PIN = 4;
constexpr uint8_t BUZZER_PIN = 21;
constexpr uint8_t ULTRA_TRIG_PIN = 18;
constexpr uint8_t ULTRA_ECHO_PIN = 19;
// GPIO18 already serves the ultrasonic TRIG pin. Use GPIO23 for a separate
// low-current indicator/load LED with a series 220-330 ohm resistor.
// This output controls an LED electrically, NOT an AC mains power supply.
constexpr uint8_t LOAD_LED_PIN = 23;

// Set the buzzer type you physically own:
// ACTIVE_HIGH: two-pin low-current 3.3V active buzzer (+ to GPIO21, - to GND).
// ACTIVE_LOW: reserved for special low-trigger modules (not used by the 2-pin dashboard).
// PASSIVE_PWM: passive/piezo buzzer that needs a tone signal.
enum BuzzerMode : uint8_t { ACTIVE_HIGH, ACTIVE_LOW, PASSIVE_PWM };
BuzzerMode buzzerMode = ACTIVE_HIGH; // Can also be changed from the phone dashboard.
constexpr uint16_t BUZZER_FREQUENCY_HZ = 2200;
constexpr uint8_t BUZZER_LEDC_CHANNEL_V2 = 0; // Arduino-ESP32 2.x only

const char *AP_SSID = "S.E.C.U.R.E";
const char *AP_PASSWORD = AQUAGUARD_AP_PASSWORD;  // must be at least 8 characters

// All three statuses are determined ONLY by ultrasonic distance, not percent.
// Requested whole-centimeter labels: SAFE 15-11, WARNING 10-7, CRITICAL 6-0.
// For precise readings, values between ranges are assigned conservatively:
// e.g. 10.9 cm -> WARNING and 6.9 cm -> CRITICAL.
// Beyond 15 cm stays SAFE, as the water is even farther from the sensor.
// Typical HC-SR04 modules cannot measure below ~2 cm; no valid echo -> FAULT.
constexpr float SAFE_FROM_CM = 11.0f;
constexpr float WARNING_FROM_CM = 7.0f;
constexpr float DISPLAY_EMPTY_CM = 15.0f;
constexpr float DISPLAY_FULL_CM = 0.0f;
float changeAlertCm = 1.0f; // Distance-change logging threshold, adjustable in dashboard.
constexpr uint32_t CHANGE_ALERT_COOLDOWN_MS = 950;
constexpr uint32_t WARNING_BUZZER_PULSE_MS = 500;
constexpr uint32_t CRITICAL_BUZZER_PULSE_MS = 200;
constexpr uint32_t CLOUD_TELEMETRY_INTERVAL_MS = 1000;

constexpr uint32_t SAMPLE_INTERVAL_MS = 420;
constexpr uint32_t ECHO_TIMEOUT_US = 27000;
constexpr float SOUND_SPEED_CM_PER_US = 0.0343f;
constexpr size_t CLOUD_TELEMETRY_JSON_SIZE = 768;
constexpr size_t CLOUD_COMMAND_RESPONSE_SIZE = 256;

struct CloudTelemetryRequest {
  char json[CLOUD_TELEMETRY_JSON_SIZE];
};

struct CloudCommandResponse {
  char body[CLOUD_COMMAND_RESPONSE_SIZE];
};

WebServer server(80);
DNSServer dns;
Preferences prefs;
QueueHandle_t cloudTelemetryQueue = nullptr;
QueueHandle_t cloudCommandResponseQueue = nullptr;

bool ultrasonicOk = false;
float distanceCm = -1.0f;
float levelPercent = -1.0f;
float waterHeightCm = -1.0f;
uint8_t echoMisses = 0;
float recentDistances[5] = {0};
uint8_t distanceCount = 0, distanceIndex = 0;

String status = "fault"; // safe / warning / critical / fault
bool buzzerOn = false;
bool buzzerEnabled = true;
uint32_t buzzerTestUntilMs = 0; // manual dashboard alarm test is time-limited
bool greenOn = false, yellowOn = false, redOn = false;
// SAFE may turn the load LED ON until a CRITICAL reading automatically shuts it
// off. That automatic shutdown is latched until the user manually turns it on.
bool loadLedOn = false;
bool loadLedAutoOffLatched = false;
bool loadLedOffTimerActive = true;
uint32_t loadLedOffSinceMs = 0;
uint32_t lastSampleMs = 0;
uint32_t lastCloudTelemetryMs = 0;
uint32_t statusSinceMs = 0;
float lastChangeReferenceCm = -1.0f;
uint32_t lastChangeAlertMs = 0;
uint32_t distanceChangeAlertCount = 0;
float lastAlertDeltaCm = -1.0f;
float lastAlertDistanceCm = -1.0f;
uint32_t lastAlertAtMs = 0;
bool lastAlertWaterRising = false;

// Complete embedded offline dashboard (no extra sketch tabs or libraries).
const char DASHBOARD_HTML[] PROGMEM = R"AQHTML(
<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<meta name="theme-color" content="#0b1329">
<title>S.E.C.U.R.E APPLICATION | Professional Water Level Telemetry</title>
<style>
:root{
  --font-sans:-apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,"Inter",Helvetica,Arial,sans-serif;
  --font-mono:ui-monospace,SFMono-Regular,Menlo,Monaco,Consolas,"Liberation Mono",monospace;
  --bg-app:#f8fafc;
  --bg-card:#ffffff;
  --border-subtle:#e2e8f0;
  --border-card:#e2e8f0;
  --ink-primary:#0f172a;
  --ink-secondary:#475569;
  --ink-muted:#64748b;
  --ink-light:#94a3b8;
  --navy-dark:#0b1329;
  --navy-surface:#0f1d38;
  --navy-card:#132347;
  --navy-border:#1e3561;
  --blue-brand:#2563eb;
  --blue-hover:#1d4ed8;
  --blue-soft:#eff6ff;
  --safe-base:#10b981;
  --safe-surface:#ecfdf5;
  --safe-text:#065f46;
  --safe-glow:rgba(16,185,129,.35);
  --warn-base:#f59e0b;
  --warn-surface:#fffbeb;
  --warn-text:#92400e;
  --warn-glow:rgba(245,158,11,.35);
  --danger-base:#ef4444;
  --danger-surface:#fef2f2;
  --danger-text:#991b1b;
  --danger-glow:rgba(239,68,68,.35);
  --radius-sm:8px;
  --radius-md:12px;
  --radius-lg:16px;
  --radius-xl:20px;
  --shadow-sm:0 1px 3px rgba(15,23,42,.06);
  --shadow-card:0 4px 14px rgba(15,23,42,.05),0 1px 3px rgba(15,23,42,.03);
  --shadow-elevated:0 14px 34px rgba(15,23,42,.08),0 4px 10px rgba(15,23,42,.04);
}
*{box-sizing:border-box}
html{scroll-behavior:smooth}
body{
  margin:0;
  font-family:var(--font-sans);
  background:var(--bg-app);
  color:var(--ink-primary);
  font-size:14px;
  line-height:1.5;
  -webkit-font-smoothing:antialiased;
  display:flex;
  height:100vh;
  height:100dvh;
  flex-direction:column;
  overflow:hidden;
}
button,input,select{font:inherit}
button{cursor:pointer}
button:disabled{cursor:not-allowed;opacity:.55}
header{
  position:sticky;
  top:0;
  z-index:1200;
  background:var(--navy-dark);
  color:#fff;
  border-bottom:1px solid rgba(255,255,255,.08);
  box-shadow:0 4px 20px rgba(0,0,0,.2);
  flex:none;
}
.nav{
  max-width:1600px;
  margin:0 auto;
  min-height:72px;
  padding:12px clamp(16px,3vw,32px);
  display:flex;
  flex-direction:row;
  align-items:center;
  justify-content:space-between;
  gap:16px;
  width:100%;
  box-sizing:border-box;
}

/* On Desktop: Brand Layer is inline */
.header-layer-brand{
  display:flex;
  align-items:center;
  gap:16px;
  min-width:0;
  border-bottom:none;
  padding:0;
  min-height:auto;
}
.brand{
  display:flex;
  align-items:center;
  gap:12px;
  min-width:0;
  text-decoration:none;
}
.logo{
  width:44px;
  height:44px;
  border-radius:12px;
  background:linear-gradient(135deg,#0284c7,#2563eb);
  color:#fff;
  border:2px solid #38bdf8;
  box-shadow:0 0 14px rgba(56,189,248,.35);
  flex:none;
  display:grid;
  place-items:center;
  font-size:20px;
  font-weight:900;
}
.brand-copy{
  display:flex;
  flex-direction:column;
  min-width:0;
}
.brand-title-row{
  display:flex;
  align-items:center;
  gap:8px;
  flex-wrap:nowrap;
}
.brand strong{
  font-size:clamp(16px,2vw,18px);
  font-weight:800;
  letter-spacing:-.3px;
  color:#fff;
  white-space:nowrap;
}
.brand-badge{
  font-size:9px;
  font-weight:800;
  letter-spacing:.8px;
  padding:2px 7px;
  border-radius:6px;
  background:rgba(56,189,248,.18);
  color:#7dd3fc;
  border:1px solid rgba(56,189,248,.3);
  text-transform:uppercase;
  white-space:nowrap;
}
.brand small{
  color:#94a3b8;
  font-size:10px;
  font-weight:700;
  letter-spacing:1px;
  text-transform:uppercase;
  margin-top:2px;
  white-space:nowrap;
}
.header-status-badge{
  display:none;
}

/* On Desktop: Tools Layer is inline */
.header-layer-tools{
  display:flex;
  align-items:center;
  padding:0;
  background:transparent;
  min-height:auto;
  border-bottom:none;
  flex:0 0 auto;
}
.header-tools{
  display:inline-flex;
  align-items:center;
  gap:10px;
  flex:0 0 auto;
}

.header-btn-icon{
  width:16px;
  height:16px;
  stroke-width:2.2;
  display:block;
  flex:none;
}
.power-source-toggle{
  height:38px;
  min-height:38px;
  max-height:38px;
  box-sizing:border-box;
  display:inline-flex;
  align-items:center;
  gap:8px;
  padding:0 12px;
  border:1px solid rgba(255,255,255,.2);
  border-radius:10px;
  background:rgba(255,255,255,.08);
  color:#e2e8f0;
  font-size:12px;
  font-weight:700;
  white-space:nowrap;
  cursor:pointer;
  flex:none;
  transition:all .18s ease;
}
.power-source-toggle:hover:not(:disabled){
  background:rgba(255,255,255,.15);
  border-color:rgba(255,255,255,.35);
  box-shadow:0 2px 10px rgba(0,0,0,.25);
}
.power-source-toggle:focus-visible{outline:2px solid #60a5fa;outline-offset:2px}
.power-source-toggle:disabled{opacity:.6;cursor:not-allowed}
.power-switch-track{
  width:32px;
  height:18px;
  border-radius:18px;
  background:#475569;
  position:relative;
  flex:none;
  transition:background .2s cubic-bezier(.4,0,.2,1);
}
.power-switch-track::after{
  content:"";
  position:absolute;
  top:2px;
  left:2px;
  width:14px;
  height:14px;
  border-radius:50%;
  background:#fff;
  box-shadow:0 1px 3px rgba(0,0,0,.3);
  transition:transform .2s cubic-bezier(.4,0,.2,1);
}
.power-source-toggle[aria-pressed="true"] .power-switch-track{
  background:var(--safe-base);
  box-shadow:0 0 10px var(--safe-glow);
}
.power-source-toggle[aria-pressed="true"] .power-switch-track::after{
  transform:translateX(14px);
}
.alert-sound-button, .notification-button{
  width:38px;
  min-width:38px;
  height:38px;
  min-height:38px;
  max-height:38px;
  box-sizing:border-box;
  display:inline-flex;
  align-items:center;
  justify-content:center;
  border:1px solid rgba(255,255,255,.2);
  border-radius:10px;
  background:rgba(255,255,255,.08);
  color:#e2e8f0;
  position:relative;
  cursor:pointer;
  flex:none;
  transition:all .18s ease;
}
.alert-sound-button:hover, .notification-button:hover{
  background:rgba(255,255,255,.16);
  color:#38bdf8;
  border-color:rgba(56,189,248,.55);
}
.alert-sound-button svg, .notification-button svg{
  width:20px;
  height:20px;
  stroke-width:2;
  display:block;
  flex:none;
}
.alert-sound-button .sound-waves{display:none}
.alert-sound-button.sound-on{
  color:#38bdf8;
  border-color:rgba(56,189,248,.6);
  background:rgba(56,189,248,.18);
  box-shadow:0 0 12px rgba(56,189,248,.3);
}
.alert-sound-button.sound-on .sound-waves{display:block}
.alert-sound-button.sound-on .sound-muted{display:none}
.notification-count{
  position:absolute;
  top:-5px;
  right:-5px;
  min-width:18px;
  height:18px;
  padding:0 5px;
  display:grid;
  place-items:center;
  border:2px solid var(--navy-dark);
  border-radius:18px;
  background:var(--danger-base);
  color:#fff;
  font-size:10px;
  font-weight:800;
  line-height:1;
  box-shadow:0 0 8px rgba(239,68,68,.6);
  animation:badge-pop .25s ease-out;
}
.notification-count[hidden]{display:none}
@keyframes badge-pop{0%{transform:scale(.5)}100%{transform:scale(1)}}
.connection-clock{
  height:38px;
  min-height:38px;
  max-height:38px;
  box-sizing:border-box;
  display:inline-flex;
  align-items:center;
  gap:9px;
  padding:0 12px;
  background:rgba(255,255,255,.08);
  border:1px solid rgba(255,255,255,.2);
  border-radius:10px;
  flex:none;
}
.dot{
  width:9px;
  height:9px;
  border-radius:50%;
  background:#f59e0b;
  flex:none;
  position:relative;
}
.dot.on{
  background:var(--safe-base);
  box-shadow:0 0 0 3px rgba(16,185,129,.3),0 0 10px rgba(16,185,129,.7);
  animation:beacon-pulse 2s cubic-bezier(.4,0,.6,1) infinite;
}
.dot.offline{
  background:var(--danger-base);
  box-shadow:0 0 0 3px rgba(239,68,68,.3),0 0 8px rgba(239,68,68,.6);
}
@keyframes beacon-pulse{
  0%,100%{box-shadow:0 0 0 3px rgba(16,185,129,.3),0 0 8px rgba(16,185,129,.6)}
  50%{box-shadow:0 0 0 6px rgba(16,185,129,.1),0 0 14px rgba(16,185,129,.8)}
}
.clock{
  display:flex;
  flex-direction:column;
  justify-content:center;
  text-align:right;
}
.clock strong{
  font-family:var(--font-mono);
  font-size:12px;
  font-weight:700;
  color:#ffffff;
  line-height:1.15;
  letter-spacing:0.2px;
}
.clock small{font-size:9.5px;color:#94a3b8;line-height:1.1}
.app-shell{
  display:grid;
  grid-template-columns:250px minmax(0,1fr);
  flex:1 1 0;
  min-height:0;
  height:auto;
  max-width:1600px;
  width:100%;
  margin:0 auto;
  background:var(--bg-app);
  overflow:hidden;
  transition:grid-template-columns .24s cubic-bezier(.4,0,.2,1);
}
.sidebar{
  background:#fff;
  border-right:1px solid var(--border-subtle);
  padding:20px 14px;
  display:flex;
  flex-direction:column;
  gap:8px;
  min-height:0;
  overflow-y:auto;
  overscroll-behavior:contain;
}
.sidebar-head{
  display:flex;
  align-items:center;
  justify-content:space-between;
  padding:0 8px 12px;
  border-bottom:1px solid var(--border-subtle);
  margin-bottom:6px;
}
.sidebar-label{
  font-size:11px;
  font-weight:800;
  letter-spacing:1.2px;
  color:var(--ink-muted);
  text-transform:uppercase;
}
.sidebar-menu-toggle{
  width:34px;
  height:34px;
  display:grid;
  place-items:center;
  border:1px solid var(--border-subtle);
  border-radius:var(--radius-sm);
  background:transparent;
  color:var(--ink-secondary);
  transition:all .15s ease;
}
.sidebar-menu-toggle:hover{
  background:var(--bg-app);
  color:var(--blue-brand);
  border-color:var(--blue-brand);
}
.sidebar-menu-toggle svg{width:16px;height:16px}
.side-nav{display:flex;flex-direction:column;gap:6px}
.side-link{
  width:100%;
  display:flex;
  align-items:center;
  gap:12px;
  border:1px solid transparent;
  border-radius:var(--radius-md);
  padding:12px 14px;
  text-align:left;
  background:transparent;
  color:var(--ink-secondary);
  font-size:13px;
  font-weight:700;
  transition:all .15s ease;
}
.side-link:hover{background:#f1f5f9;color:var(--ink-primary)}
.side-link[aria-pressed="true"]{
  background:var(--blue-soft);
  color:var(--blue-brand);
  border-color:rgba(37,99,235,.2);
  box-shadow:inset 3px 0 0 var(--blue-brand);
}
.side-icon{width:22px;display:grid;place-items:center;font-size:16px;color:currentColor}
.sidebar-note{
  margin-top:auto;
  padding:14px;
  border-radius:var(--radius-md);
  background:#f8fafc;
  border:1px solid var(--border-subtle);
  color:var(--ink-muted);
  font-size:11px;
  line-height:1.6;
}
.app-shell.sidebar-collapsed{grid-template-columns:74px minmax(0,1fr)}
.sidebar-collapsed .sidebar{padding:20px 8px;align-items:center}
.sidebar-collapsed .sidebar-head{width:100%;justify-content:center;padding:0 0 12px}
.sidebar-collapsed .sidebar-label,
.sidebar-collapsed .sidebar-note,
.sidebar-collapsed .side-link span:last-child{display:none}
.sidebar-collapsed .side-link{justify-content:center;padding:12px}
main{
  min-height:0;
  min-width:0;
  overflow-y:auto;
  overscroll-behavior:contain;
  scroll-behavior:smooth;
  -webkit-overflow-scrolling:touch;
  padding:28px clamp(16px,3vw,36px) 40px;
}
.app-view[hidden]{display:none}
.pagehead{
  display:flex;
  align-items:flex-end;
  justify-content:space-between;
  gap:16px;
  flex-wrap:wrap;
  margin-bottom:24px;
}
.eyebrow{
  color:var(--blue-brand);
  font-size:11px;
  font-weight:800;
  letter-spacing:1.5px;
  text-transform:uppercase;
  display:flex;
  align-items:center;
  gap:6px;
}
.eyebrow::before{content:"";width:6px;height:6px;border-radius:50%;background:currentColor}
h1{
  font-size:clamp(24px,3.2vw,32px);
  font-weight:850;
  letter-spacing:-.8px;
  color:var(--ink-primary);
  margin:6px 0 4px;
}
.sub{color:var(--ink-muted);font-size:13px;margin:0;max-width:600px}
.updated{
  color:var(--ink-secondary);
  font-size:12px;
  font-weight:600;
  background:#fff;
  border:1px solid var(--border-subtle);
  padding:8px 14px;
  border-radius:var(--radius-full);
  box-shadow:var(--shadow-sm);
  display:inline-flex;
  align-items:center;
  gap:7px;
}
.updated::before{content:"";width:7px;height:7px;border-radius:50%;background:#3b82f6;box-shadow:0 0 6px #3b82f6}
.card{
  background:var(--bg-card);
  border:1px solid var(--border-card);
  border-radius:var(--radius-lg);
  padding:24px;
  box-shadow:var(--shadow-card);
  min-width:0;
  position:relative;
}
.cols{display:grid;grid-template-columns:1.15fr .85fr;gap:20px}
.card-title{
  color:var(--ink-muted);
  font-size:11px;
  font-weight:800;
  letter-spacing:1.2px;
  text-transform:uppercase;
  margin:0 0 8px;
}
.status-card{
  min-height:300px;
  background:linear-gradient(145deg,#0a152d 0%,#0f2244 55%,#132b57 100%);
  color:#fff;
  border:1px solid #1e3a6a;
  box-shadow:0 12px 36px rgba(11,21,45,.2),0 0 0 1px rgba(255,255,255,.05);
  display:flex;
  flex-direction:column;
  justify-content:space-between;
  overflow:hidden;
  position:relative;
}
.status-card::before{
  content:"";
  position:absolute;
  inset:0;
  background:radial-gradient(circle at 85% 15%,rgba(56,189,248,.15),transparent 60%);
  pointer-events:none;
}
.status-card .card-title{color:#93c5fd}
.status-title{
  margin:20px 0 10px;
  font-size:clamp(28px,4vw,40px);
  font-weight:900;
  line-height:1.1;
  letter-spacing:-1px;
  position:relative;
  z-index:1;
}
.status-title.safe{color:#6ee7b7;text-shadow:0 0 24px rgba(16,185,129,.4)}
.status-title.warning{color:#fde047;text-shadow:0 0 24px rgba(245,158,11,.45)}
.status-title.critical{color:#fca5a5;text-shadow:0 0 24px rgba(239,68,68,.5)}
.status-title.fault{color:#fed7aa}
.status-title.offline{color:#cbd5e1}
.status-desc{color:#cbd5e1;font-size:13px;line-height:1.65;max-width:460px;margin:0;position:relative;z-index:1}
.condition{
  margin-top:20px;
  font-family:var(--font-mono);
  font-size:12px;
  color:#93c5fd;
  background:rgba(15,34,68,.7);
  border:1px solid rgba(147,197,253,.25);
  padding:10px 14px;
  border-radius:var(--radius-md);
  align-self:flex-start;
  display:inline-flex;
  align-items:center;
  gap:8px;
  backdrop-filter:blur(6px);
  position:relative;
  z-index:1;
}
.condition::before{content:"";width:7px;height:7px;border-radius:50%;background:#38bdf8;box-shadow:0 0 6px #38bdf8}
.level-card{
  display:flex;
  align-items:center;
  justify-content:space-between;
  gap:20px;
  min-height:300px;
}
.level-meta{flex:1;min-width:0}
.metric{
  margin:12px 0 6px;
  display:flex;
  align-items:baseline;
  gap:8px;
  flex-wrap:wrap;
}
.metric strong{
  font-size:clamp(42px,5.5vw,62px);
  font-weight:900;
  letter-spacing:-2px;
  color:var(--ink-primary);
  line-height:1;
  font-family:var(--font-mono);
}
.metric span{
  color:var(--ink-muted);
  font-size:14px;
  font-weight:700;
  text-transform:uppercase;
  letter-spacing:.8px;
}
.helper{color:var(--ink-muted);font-size:12px;line-height:1.55;max-width:320px}
.mini{
  margin-top:18px;
  display:grid;
  gap:8px;
  font-size:12px;
  padding-top:14px;
  border-top:1px solid var(--border-subtle);
}
.mini div{display:flex;justify-content:space-between;align-items:center;gap:12px}
.mini span{color:var(--ink-muted)}
.mini b{color:var(--ink-primary);font-family:var(--font-mono);font-size:13px}
.home-illustration{
  width:76px;
  height:76px;
  flex:none;
  display:grid;
  place-items:center;
  border-radius:16px;
  background:#f0faff;
  color:#42bfe5;
}
.home-illustration svg{
  width:42px;
  height:42px;
}
.stats-title{
  display:flex;
  justify-content:space-between;
  align-items:center;
  gap:12px;
  margin:32px 0 14px;
}
.stats-title h2{font-size:16px;font-weight:800;letter-spacing:-.3px;margin:0;color:var(--ink-primary)}
.stats-title span{color:var(--ink-muted);font-size:12px;font-weight:600}
.sensors{display:grid;grid-template-columns:repeat(4,1fr);gap:16px}
.sensor{
  background:#fff;
  border:1px solid var(--border-card);
  border-radius:var(--radius-lg);
  padding:20px;
  box-shadow:var(--shadow-card);
  display:flex;
  flex-direction:column;
  justify-content:space-between;
  transition:all .18s ease;
}
.sensor:hover{transform:translateY(-2px);box-shadow:var(--shadow-elevated);border-color:#cbd5e1}
.sensor small{
  color:var(--ink-muted);
  font-size:11px;
  font-weight:800;
  letter-spacing:1px;
  text-transform:uppercase;
  display:block;
  margin-bottom:8px;
}
.sensor strong{
  font-size:19px;
  font-weight:850;
  color:var(--ink-primary);
  letter-spacing:-.5px;
  font-family:var(--font-mono);
  overflow-wrap:anywhere;
}
.sensor .secondary{
  color:var(--ink-muted);
  font-size:11px;
  margin-top:10px;
  padding-top:10px;
  border-top:1px dashed var(--border-subtle);
}
.lower{display:grid;grid-template-columns:1.25fr .75fr;gap:20px;margin-top:20px}
.charthead{display:flex;align-items:center;justify-content:space-between;gap:8px;margin-bottom:18px}
.charthead b{font-size:15px;font-weight:800;color:var(--ink-primary)}
.charthead span{
  color:var(--ink-muted);
  font-size:11px;
  font-weight:600;
  padding:3px 8px;
  border-radius:var(--radius-full);
  background:#f1f5f9;
}
canvas{
  width:100%;
  height:190px;
  display:block;
  border-radius:var(--radius-md);
  background:#fafcff;
  border:1px solid var(--border-subtle);
}
.ledrow{display:grid;grid-template-columns:repeat(3,1fr);gap:12px;margin:14px 0 20px}
.lightbox{
  border-radius:var(--radius-md);
  background:#f8fafc;
  padding:14px 10px;
  text-align:center;
  border:1px solid var(--border-subtle);
  transition:all .15s ease;
}
.led{
  display:block;
  width:32px;
  height:32px;
  border-radius:50%;
  background:#cbd5e1;
  margin:4px auto 10px;
  box-shadow:inset 0 2px 4px rgba(0,0,0,.2),0 0 0 4px #e2e8f0;
  transition:all .2s ease;
}
.led.green.on{
  background:radial-gradient(circle at 35% 35%,#6ee7b7,#10b981 70%,#059669);
  box-shadow:0 0 0 4px #d1fae5,0 0 22px rgba(16,185,129,.85);
}
.led.yellow.on{
  background:radial-gradient(circle at 35% 35%,#fde047,#f59e0b 70%,#d97706);
  box-shadow:0 0 0 4px #fef3c7,0 0 22px rgba(245,158,11,.85);
}
.led.red.on{
  background:radial-gradient(circle at 35% 35%,#fca5a5,#ef4444 70%,#b91c1c);
  box-shadow:0 0 0 4px #fee2e2,0 0 22px rgba(239,68,68,.85);
}
.lightbox b{font-size:12px;font-weight:700;color:var(--ink-primary)}
.hrow{
  display:flex;
  align-items:center;
  justify-content:space-between;
  padding:11px 0;
  border-bottom:1px solid var(--border-subtle);
  font-size:12px;
  gap:10px;
}
.hrow:last-child{border-bottom:0}
.hrow span{color:var(--ink-secondary)}
.hrow strong{color:var(--ink-primary);font-family:var(--font-mono);font-weight:700}
.hrow strong.alarm{color:var(--danger-base);font-weight:800}
.test-panel{border-top:1px solid var(--border-subtle);margin-top:16px;padding-top:16px}
.buzzer-toggle-ui{
  display:flex;
  align-items:center;
  justify-content:space-between;
  gap:16px;
  padding:12px 14px;
  background:#f8fafc;
  border:1px solid var(--border-subtle);
  border-radius:var(--radius-md);
}
.buzzer-toggle-copy{display:grid;gap:2px}
.buzzer-toggle-copy strong{color:var(--ink-primary);font-size:13px;font-weight:750}
.buzzer-toggle-copy span{color:var(--ink-muted);font-size:11px}
.buzzer-toggle-button{
  display:inline-flex;
  align-items:center;
  gap:8px;
  min-height:38px;
  padding:6px 12px;
  border:1px solid var(--border-subtle);
  border-radius:var(--radius-md);
  background:#fff;
  color:var(--ink-secondary);
  font-weight:750;
  font-size:12px;
  box-shadow:var(--shadow-sm);
  transition:all .18s ease;
}
.buzzer-toggle-button[aria-pressed="true"]{
  background:var(--safe-surface);
  color:var(--safe-text);
  border-color:rgba(16,185,129,.3);
}
.buzzer-toggle-track{
  width:32px;
  height:18px;
  border-radius:18px;
  background:#cbd5e1;
  position:relative;
  flex:none;
  transition:background .2s ease;
}
.buzzer-toggle-track::after{
  content:"";
  position:absolute;
  top:2px;
  left:2px;
  width:14px;
  height:14px;
  border-radius:50%;
  background:#fff;
  box-shadow:0 1px 3px rgba(0,0,0,.25);
  transition:transform .2s ease;
}
.buzzer-toggle-button[aria-pressed="true"] .buzzer-toggle-track{background:var(--safe-base)}
.buzzer-toggle-button[aria-pressed="true"] .buzzer-toggle-track::after{transform:translateX(14px)}
.buzzer-toggle-message{min-height:18px;margin-top:8px;color:var(--ink-muted);font-size:12px}
.buzzer-toggle-message.error{color:var(--danger-base)}
select.driver-select{
  width:100%;
  border:1px solid var(--border-subtle);
  border-radius:var(--radius-md);
  padding:10px 12px;
  background:#fff;
  color:var(--ink-primary);
  margin-top:6px;
}
.btn{
  border:0;
  background:var(--blue-brand);
  color:#fff;
  padding:11px 18px;
  border-radius:var(--radius-md);
  font-size:13px;
  font-weight:750;
  white-space:nowrap;
  box-shadow:0 2px 6px rgba(37,99,235,.25);
  transition:all .15s ease;
}
.btn:hover:not(:disabled){background:var(--blue-hover);box-shadow:0 4px 12px rgba(37,99,235,.35)}
.btn:focus-visible{outline:2px solid #60a5fa;outline-offset:2px}
.hint{color:var(--ink-muted);font-size:12px;line-height:1.6}
.message{min-height:20px;margin-top:10px;font-size:12px;color:var(--ink-muted)}
.message.error{color:var(--danger-base);font-weight:700}
.message.ok{color:var(--safe-base);font-weight:700}
.cal-head{margin-top:24px;margin-bottom:14px}
.cal-head h2{margin:0;font-size:18px;font-weight:800;color:var(--ink-primary)}
.cal-head p{margin:4px 0 0;color:var(--ink-muted);font-size:13px}
.config-grid{display:grid;grid-template-columns:repeat(4,minmax(0,1fr));gap:16px;margin-top:20px}
.field label{display:block;color:var(--ink-secondary);font-weight:700;font-size:12px;margin-bottom:8px}
.field input{
  border:1px solid var(--border-subtle);
  border-radius:var(--radius-md);
  padding:11px 14px;
  width:100%;
  color:var(--ink-primary);
  background:#fff;
  font-size:14px;
  font-family:var(--font-mono);
  outline:none;
  transition:border-color .15s,box-shadow .15s;
}
.field input:focus{border-color:var(--blue-brand);box-shadow:0 0 0 3px rgba(37,99,235,.15)}
.settings-bottom{
  display:flex;
  align-items:center;
  justify-content:space-between;
  gap:16px;
  flex-wrap:wrap;
  margin-top:20px;
  padding-top:16px;
  border-top:1px solid var(--border-subtle);
}
dialog{border:none;background:transparent;padding:0}
dialog::backdrop{background:rgba(11,19,41,.6);backdrop-filter:blur(6px)}
.power-dialog{
  position:fixed;
  z-index:2000;
  inset:84px 20px auto auto;
  margin:0;
  width:min(420px,calc(100vw - 32px));
  max-height:calc(100vh - 100px);
  overflow-y:auto;
  border-radius:var(--radius-xl);
  padding:24px;
  background:#fff;
  color:var(--ink-primary);
  box-shadow:var(--shadow-elevated),0 0 0 1px rgba(0,0,0,.05);
}
.power-dialog h2{margin:0 0 8px;font-size:20px;font-weight:850;color:var(--ink-primary)}
.power-dialog p{color:var(--ink-muted);font-size:13px;line-height:1.6;margin:0}
.power-actions{display:grid;grid-template-columns:1fr 1fr;gap:12px;margin-top:20px}
.power-action{border:0;border-radius:var(--radius-md);padding:12px;font-size:13px;font-weight:800;transition:all .15s ease}
.power-action.action-off{background:var(--danger-base);color:#fff;box-shadow:0 2px 8px rgba(239,68,68,.35)}
.power-action.action-off:hover:not(:disabled){background:#dc2626}
.power-action.action-on{background:var(--safe-base);color:#fff;box-shadow:0 2px 8px rgba(16,185,129,.35)}
.power-action.action-on:hover:not(:disabled){background:#059669}
.history-dialog{
  position:fixed;
  z-index:2000;
  inset:84px 20px auto auto;
  margin:0;
  width:min(520px,calc(100vw - 32px));
  max-height:calc(100vh - 100px);
  border-radius:var(--radius-xl);
  padding:0;
  background:#fff;
  color:var(--ink-primary);
  box-shadow:var(--shadow-elevated),0 0 0 1px rgba(0,0,0,.05);
  overflow:hidden;
}
.history-head{
  display:flex;
  align-items:flex-start;
  justify-content:space-between;
  gap:16px;
  padding:20px 24px;
  border-bottom:1px solid var(--border-subtle);
  background:#f8fafc;
}
.history-head h2{margin:0;font-size:18px;font-weight:850;color:var(--ink-primary)}
.history-head p{margin:4px 0 0;color:var(--ink-muted);font-size:12px}
.history-close{
  width:32px;
  height:32px;
  border:1px solid var(--border-subtle);
  border-radius:var(--radius-sm);
  background:#fff;
  color:var(--ink-secondary);
  font-size:18px;
  display:grid;
  place-items:center;
  line-height:1;
  transition:all .15s ease;
}
.history-close:hover{background:#f1f5f9;color:var(--ink-primary)}
.history-list{max-height:calc(100vh - 220px);overflow-y:auto;padding:8px 12px 14px}
.history-empty{padding:36px 16px;text-align:center;color:var(--ink-muted);font-size:13px}
.history-item{
  width:100%;
  display:block;
  padding:14px;
  text-align:left;
  border:0;
  border-bottom:1px solid var(--border-subtle);
  background:#fff;
  border-radius:var(--radius-md);
  color:inherit;
  transition:background .15s ease;
}
.history-item:last-child{border-bottom:0}
.history-item:not(:disabled):hover{background:#f8fafc}
.history-item-head{display:flex;align-items:center;justify-content:space-between;gap:10px}
.history-item-head strong{font-size:13px;font-weight:800;color:var(--ink-primary)}
.history-kind{padding:3px 8px;border-radius:var(--radius-full);font-size:10px;font-weight:800;text-transform:uppercase;letter-spacing:.5px}
.history-kind.safe{background:var(--safe-surface);color:var(--safe-text)}
.history-kind.warning{background:var(--warn-surface);color:var(--warn-text)}
.history-kind.critical{background:var(--danger-surface);color:var(--danger-text)}
.history-kind.fault{background:#f1f5f9;color:var(--ink-secondary)}
.history-item time{display:block;margin-top:4px;color:var(--ink-light);font-size:11px;font-family:var(--font-mono)}
.history-item-description{display:block;margin-top:6px;color:var(--ink-secondary);font-size:12px;line-height:1.5}
.notification-slot{display:contents}
.toast{
  position:fixed;
  top:84px;
  right:20px;
  z-index:3000;
  width:min(400px,calc(100vw - 32px));
  margin:0;
  display:flex;
  align-items:flex-start;
  gap:14px;
  padding:14px 16px;
  background:#fff;
  border:1px solid var(--border-card);
  border-left:5px solid var(--blue-brand);
  border-radius:var(--radius-lg);
  box-shadow:var(--shadow-elevated),0 0 0 1px rgba(0,0,0,.05);
  opacity:0;
  transform:translateY(-12px);
  transition:opacity .25s ease,transform .25s cubic-bezier(.34,1.56,.64,1);
}
.toast.show{opacity:1;transform:translateY(0)}
.toast.safe{border-left-color:var(--safe-base)}
.toast.warning{border-left-color:var(--warn-base)}
.toast.critical{border-left-color:var(--danger-base)}
.toast.fault{border-left-color:var(--ink-muted)}
.toast-icon{
  width:34px;
  height:34px;
  flex:none;
  border-radius:50%;
  display:grid;
  place-items:center;
  background:var(--blue-soft);
  color:var(--blue-brand);
  font-weight:900;
  font-size:14px;
}
.toast.safe .toast-icon{background:var(--safe-surface);color:var(--safe-base)}
.toast.warning .toast-icon{background:var(--warn-surface);color:var(--warn-base)}
.toast.critical .toast-icon{background:var(--danger-surface);color:var(--danger-base)}
.toast.fault .toast-icon{background:#f1f5f9;color:var(--ink-secondary)}
.toast-main{flex:1;min-width:0;display:flex;align-items:flex-start;gap:12px;padding:0;border:0;background:transparent;text-align:left;color:inherit}
.toast-copy strong{display:block;color:var(--ink-primary);font-size:14px;font-weight:800;margin-bottom:2px}
.toast-copy span{display:block;color:var(--ink-secondary);font-size:12px;line-height:1.5}
.toast-close{border:0;background:transparent;color:var(--ink-light);font-size:22px;line-height:1;padding:0 2px;cursor:pointer;transition:color .15s}
.toast-close:hover{color:var(--ink-primary)}
.toast[hidden]{display:none}
.footer{flex:none;width:100%;background:var(--navy-dark);color:#94a3b8;border-top:1px solid rgba(255,255,255,.08)}
.footer-inner{
  max-width:1600px;
  margin:0 auto;
  padding:14px clamp(16px,3vw,32px);
  display:flex;
  align-items:center;
  justify-content:space-between;
  flex-wrap:wrap;
  gap:10px;
}
.footer-brand strong{color:#fff;font-size:11px;letter-spacing:.8px}
.footer-brand span{color:#64748b;font-size:11px}
.preview{background:#fef3c7;color:#92400e;font-size:12px;font-weight:700;padding:8px 16px;text-align:center;border-bottom:1px solid #fde68a}
.sr-only{position:absolute;width:1px;height:1px;padding:0;margin:-1px;overflow:hidden;clip:rect(0,0,0,0);white-space:nowrap;border:0}
@media(max-width:1024px){
  .app-shell{grid-template-columns:220px minmax(0,1fr)}
  .cols,.lower{grid-template-columns:1fr}
  .sensors{grid-template-columns:repeat(2,1fr)}
  .config-grid{grid-template-columns:repeat(2,1fr)}
}
@media(max-width:768px){
  body{height:auto;min-height:100svh;overflow-x:hidden;overflow-y:auto}
  .app-shell{display:grid;grid-template-columns:minmax(0,1fr);height:auto;min-height:auto;overflow:visible;flex:none}
  .sidebar{position:static;border-right:0;border-bottom:1px solid var(--border-subtle);padding:8px 12px;height:auto;min-height:auto;overflow:visible}
  .sidebar-head,.sidebar-note{display:none}
  .side-nav{display:grid;grid-template-columns:repeat(3,minmax(0,1fr));gap:6px}
  .side-link{justify-content:center;flex-direction:column;gap:3px;padding:8px 4px;font-size:11px;text-align:center}
  .side-link[aria-pressed="true"]{box-shadow:inset 0 -3px 0 var(--blue-brand)}

  /* Two-Layer Header on Mobile */
  .nav{
    display:flex;
    flex-direction:column;
    padding:0;
    min-height:auto;
    gap:0;
  }
  .header-layer-brand{
    display:flex;
    align-items:center;
    justify-content:space-between;
    padding:10px 14px 8px;
    border-bottom:1px solid rgba(255,255,255,.08);
    min-height:52px;
    width:100%;
    box-sizing:border-box;
    gap:10px;
  }
  .brand strong{font-size:15px;white-space:nowrap}
  .brand small{font-size:9.5px;letter-spacing:.8px;display:block}
  .brand-badge{font-size:8.5px;padding:2px 6px}
  .header-status-badge{display:none}

  .header-layer-tools{
    display:flex;
    align-items:center;
    padding:8px 14px;
    background:rgba(15,29,56,.55);
    border-bottom:1px solid rgba(255,255,255,.05);
    min-height:50px;
    width:100%;
    box-sizing:border-box;
    overflow-x:auto;
    overflow-y:hidden;
    -webkit-overflow-scrolling:touch;
    scrollbar-width:none;
  }
  .header-layer-tools::-webkit-scrollbar{display:none}
  .header-tools{
    display:flex;
    align-items:center;
    gap:8px;
    width:max-content;
    min-width:100%;
  }
  .header-tools > *{flex-shrink:0}
  .header-tools > .connection-clock{margin-left:auto}

  main{overflow-y:visible;height:auto;min-height:auto;padding:18px 14px 28px}
  .footer{position:static;flex:none}
  .sensors{gap:10px}
  .status-card,.level-card{min-height:240px;padding:18px}
  .home-illustration{width:64px;height:64px}
  .home-illustration svg{width:38px;height:38px}
}
@media(max-width:480px){
  .header-layer-brand{padding:8px 12px 6px;min-height:48px}
  .logo{width:36px;height:36px;font-size:18px}
  .brand strong{font-size:14px}
  .brand small{font-size:8.5px}
  .brand-badge{font-size:8px;padding:1px 5px}

  .header-layer-tools{padding:6px 12px;min-height:46px}
  .header-tools{gap:6px}
  .power-source-toggle,
  .alert-sound-button,
  .notification-button,
  .connection-clock{
    height:36px;
    min-height:36px;
    max-height:36px;
  }
  .power-source-toggle{padding:0 8px;font-size:11px;gap:6px}
  .power-source-toggle #powerToggleState{display:none}
  .alert-sound-button,.notification-button{width:36px;min-width:36px}
  .alert-sound-button svg,.notification-button svg{width:18px;height:18px}
  .connection-clock{padding:0 8px;gap:7px}
  .clock strong{font-size:11px}
  .clock small{display:none}

  .sensors{grid-template-columns:1fr}
  .config-grid{grid-template-columns:1fr}
  .power-dialog,.history-dialog{inset:68px 10px auto;width:calc(100vw - 20px)}
}
@media(max-width:380px){
  .header-layer-brand{padding:6px 10px 5px;min-height:44px}
  .logo{width:32px;height:32px;font-size:16px}
  .brand strong{font-size:13px}
  .brand small{font-size:8px}

  .header-layer-tools{padding:5px 8px;min-height:42px}
  .header-tools{gap:5px}
  .power-source-toggle,
  .alert-sound-button,
  .notification-button,
  .connection-clock{
    height:34px;
    min-height:34px;
    max-height:34px;
  }
  .power-source-toggle{padding:0 6px;font-size:10px;gap:4px}
  .power-switch-track{width:26px;height:15px}
  .power-switch-track::after{width:11px;height:11px}
  .power-source-toggle[aria-pressed="true"] .power-switch-track::after{transform:translateX(11px)}
  .alert-sound-button,.notification-button{width:34px;min-width:34px}
  .alert-sound-button svg,.notification-button svg{width:16px;height:16px}
  .connection-clock{padding:0 6px;gap:5px}
  .clock strong{font-size:10px}
}
@media(prefers-reduced-motion:reduce){
  html{scroll-behavior:auto}
  *,*::before,*::after{animation-duration:.01ms!important;transition-duration:.01ms!important}
}
html[data-theme="dark"]{
  color-scheme:dark;
  --bg-app:#000;--bg-card:#0b0b0b;--border-subtle:#262626;--border-card:#262626;
  --ink-primary:#f5f5f5;--ink-secondary:#d4d4d4;--ink-muted:#a3a3a3;--ink-light:#737373;
  --navy-dark:#000;--navy-surface:#101010;--navy-card:#151515;--navy-border:#292929;
  --blue-soft:#111827;--safe-surface:#052e23;--safe-text:#a7f3d0;
  --warn-surface:#332500;--warn-text:#fde68a;--danger-surface:#3b0d0d;--danger-text:#fecaca;
  --shadow-sm:0 1px 3px rgba(0,0,0,.4);
  --shadow-card:0 4px 14px rgba(0,0,0,.45),0 1px 3px rgba(0,0,0,.35);
  --shadow-elevated:0 14px 34px rgba(0,0,0,.7),0 4px 10px rgba(0,0,0,.45);
}
html[data-theme="dark"] .header-layer-tools{background:rgba(18,18,18,.8);border-bottom-color:rgba(255,255,255,.04)}
html[data-theme="dark"] body,
html[data-theme="dark"] .app-shell{background:#000;color:var(--ink-primary)}
html[data-theme="dark"] .sidebar,
html[data-theme="dark"] .sensor,
html[data-theme="dark"] .updated,
html[data-theme="dark"] .lightbox,
html[data-theme="dark"] .history-item,
html[data-theme="dark"] .history-close,
html[data-theme="dark"] .buzzer-toggle-ui,
html[data-theme="dark"] .buzzer-toggle-button,
html[data-theme="dark"] select.driver-select,
html[data-theme="dark"] .field input,
html[data-theme="dark"] .power-dialog,
html[data-theme="dark"] .history-dialog,
html[data-theme="dark"] .toast{background:#0b0b0b;color:var(--ink-primary);border-color:var(--border-subtle)}
html[data-theme="dark"] .side-link:hover,
html[data-theme="dark"] .history-item:not(:disabled):hover,
html[data-theme="dark"] .history-close:hover{background:#171717;color:var(--ink-primary)}
html[data-theme="dark"] .history-head{background:#111}
html[data-theme="dark"] .history-kind.fault{background:#262626;color:#d4d4d4}
html[data-theme="dark"] .sidebar-note,
html[data-theme="dark"] .charthead span,
html[data-theme="dark"] canvas,
html[data-theme="dark"] .buzzer-toggle-button[aria-pressed="true"]{background:#111}
html[data-theme="dark"] .status-card{background:linear-gradient(145deg,#000 0%,#080808 55%,#111 100%);border-color:#292929}
html[data-theme="dark"] .level-card[data-level-status="warning"]{--level-soft:#332500}
html[data-theme="dark"] .level-card[data-level-status="critical"]{--level-soft:#3b0d0d}
html[data-theme="dark"] .level-card[data-level-status="fault"],
html[data-theme="dark"] .level-card[data-level-status="offline"]{--level-soft:#171717}
html[data-theme="dark"] .house-illustration{background:#050505}
html[data-theme="dark"] .house-illustration svg>path:first-of-type{fill:#080808;stroke:#525252}
html[data-theme="dark"] .house-illustration svg>path:last-child{stroke:#a3a3a3}
html[data-theme="dark"] .house-reading strong{color:#f5f5f5;text-shadow:0 1px 4px rgba(0,0,0,.8)}
html[data-theme="dark"] .preview{background:#332500;color:#fde68a;border-color:#5c4500}
.appearance-card{display:flex;align-items:center;justify-content:space-between;gap:20px;margin-bottom:24px}
.appearance-copy h2{margin:0;color:var(--ink-primary);font-size:16px;font-weight:800}
.appearance-copy p{margin:4px 0 0;color:var(--ink-muted);font-size:12px}
.appearance-control{display:flex;align-items:center;gap:14px}
.appearance-control>span{color:var(--ink-secondary);font-size:13px;font-weight:700}
.theme-toggle{
  display:inline-flex;align-items:center;gap:9px;min-width:82px;min-height:38px;padding:6px 10px;
  border:1px solid var(--border-subtle);border-radius:var(--radius-md);
  background:var(--bg-app);color:var(--ink-secondary);font-size:12px;font-weight:750;
}
.theme-toggle:focus-visible{outline:2px solid #60a5fa;outline-offset:2px}
.theme-toggle-track{position:relative;width:32px;height:18px;flex:none;border-radius:18px;background:#94a3b8;transition:background .2s ease}
.theme-toggle-track:after{content:"";position:absolute;top:2px;left:2px;width:14px;height:14px;border-radius:50%;background:#fff;box-shadow:0 1px 3px rgba(0,0,0,.3);transition:transform .2s ease}
.theme-toggle[aria-pressed="true"] .theme-toggle-track{background:#2563eb}
.theme-toggle[aria-pressed="true"] .theme-toggle-track:after{transform:translateX(14px)}
html[data-theme="dark"] .theme-toggle{background:#111;color:#f5f5f5}
@media(max-width:480px){.appearance-card{align-items:flex-start;flex-direction:column;gap:14px}}
</style>
</head>
<body>
<div class="preview" id="previewBanner" style="display:none">Sample screen only. These are not live readings.</div>
<dialog class="power-dialog" id="powerDialog" aria-labelledby="powerDialogTitle" aria-describedby="powerDialogDescription">
  <h2 id="powerDialogTitle">Water warning</h2>
  <p id="powerDialogDescription"></p>
  <div class="power-actions">
    <button class="power-action action-off" id="powerOffButton" type="button" aria-label="Turn Power Source off">TURN OFF</button>
    <button class="power-action action-on" id="powerOnButton" type="button" aria-label="Turn Power Source on">TURN ON</button>
  </div>
</dialog>
<dialog class="history-dialog" id="notificationHistory" aria-labelledby="historyTitle">
  <div class="history-head">
    <div>
      <h2 id="historyTitle">Alerts</h2>
      <p id="historySummary">No alerts yet</p>
    </div>
    <button class="history-close" id="historyClose" type="button" aria-label="Close alerts">&times;</button>
  </div>
  <div class="history-list" id="notificationHistoryList" aria-live="polite">
    <div class="history-empty">No alerts yet. New alerts will show here.</div>
  </div>
</dialog>
<header>
  <div class="nav">
    <!-- Layer 1: Application Identity -->
    <div class="header-layer-brand">
      <div class="brand">
        <div class="logo">≋</div>
        <div class="brand-copy">
          <div class="brand-title-row">
            <strong>S.E.C.U.R.E APPLICATION</strong>
            <span class="brand-badge">Device AP</span>
          </div>
          <small>WATER LEVEL MONITORING SYSTEM</small>
        </div>
      </div>
      <div class="header-status-badge" aria-label="System status: Device Active">
        <span class="status-beacon" aria-hidden="true"></span>
        <span class="status-beacon-text">DEVICE AP</span>
      </div>
    </div>
    <!-- Layer 2: Action Icons & Telemetry Controls -->
    <div class="header-layer-tools">
      <div class="header-tools">
        <button class="power-source-toggle" id="powerSourceToggle" type="button" aria-label="Power Source is off" aria-pressed="false" disabled>
          <span>Power Source</span>
          <span class="power-switch-track" aria-hidden="true"></span>
          <span id="powerToggleState">Off</span>
        </button>
        <span class="sr-only" id="powerToggleMessage" role="status" aria-live="polite"></span>
        <button class="alert-sound-button" id="alertSoundButton" type="button" aria-label="Turn on alert sound" aria-pressed="false" title="Turn on alert sound">
          <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">
            <path d="M11 5 6 9H3v6h3l5 4V5Z"/>
            <path class="sound-waves" d="M15.5 8.5a5 5 0 0 1 0 7M18.5 5.5a9 9 0 0 1 0 13"/>
            <path class="sound-muted" d="m16 9 5 6m0-6-5 6"/>
          </svg>
        </button>
        <span class="sr-only" id="alertSoundStatus" role="status" aria-live="polite">Sound is off. Tap the sound button to enable alert sounds.</span>
        <button class="notification-button" id="notificationButton" type="button" aria-label="Alerts: 0" title="Show alerts">
          <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">
            <path d="M18 8a6 6 0 0 0-12 0c0 7-3 7-3 9h18c0-2-3-2-3-9"/>
            <path d="M10 21h4"/>
          </svg>
          <span class="notification-count" id="notificationCount" hidden>0</span>
        </button>
        <div class="connection-clock" role="status" aria-label="Connection status and local date and time">
          <span class="dot" id="linkDot" aria-label="Connecting"></span>
          <div class="clock" aria-label="Local date and time">
            <strong id="clockTime">--:-- --</strong>
            <small id="clockDate">Loading date...</small>
          </div>
        </div>
      </div>
    </div>
  </div>
</header>
<div class="notification-slot">
  <aside class="toast" id="statusToast" aria-live="polite" aria-atomic="true" hidden>
    <button class="toast-main" id="toastDetails" type="button" disabled>
      <span class="toast-icon" id="toastIcon">!</span>
      <span class="toast-copy">
        <strong id="toastTitle"></strong>
        <span id="toastMessage"></span>
      </span>
    </button>
    <button class="toast-close" id="toastClose" type="button" aria-label="Close notification">&times;</button>
  </aside>
</div>
<div class="app-shell" id="appShell">
  <aside class="sidebar">
    <div class="sidebar-head">
      <div class="sidebar-label">Navigation</div>
      <button class="sidebar-menu-toggle" id="menuToggle" type="button" aria-label="Collapse menu" aria-expanded="true" aria-controls="sideNav" title="Collapse menu">
        <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">
          <path d="M4 6h16M4 12h16M4 18h16"/>
        </svg>
        <span class="sidebar-toggle-text sr-only">Hide menu</span>
      </button>
    </div>
    <nav class="side-nav" id="sideNav" aria-label="Main menu">
      <button class="side-link" type="button" data-view="dashboardView" aria-controls="dashboardView" aria-pressed="true">
        <span class="side-icon" aria-hidden="true">
          <svg viewBox="0 0 24 24" width="18" height="18" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="m3 9 9-7 9 7v11a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2z"/><polyline points="9 22 9 12 15 12 15 22"/></svg>
        </span>
        <span>Home</span>
      </button>
      <button class="side-link" type="button" data-view="monitoringView" aria-controls="monitoringView" aria-pressed="false">
        <span class="side-icon" aria-hidden="true">
          <svg viewBox="0 0 24 24" width="18" height="18" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><polyline points="22 12 18 12 15 21 9 3 6 12 2 12"/></svg>
        </span>
        <span>Water history</span>
      </button>
      <button class="side-link" type="button" data-view="settingsView" aria-controls="settingsView" aria-pressed="false">
        <span class="side-icon" aria-hidden="true">
          <svg viewBox="0 0 24 24" width="18" height="18" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><circle cx="12" cy="12" r="3"/><path d="M19.4 15a1.65 1.65 0 0 0 .33 1.82l.06.06a2 2 0 0 1 0 2.83 2 2 0 0 1-2.83 0l-.06-.06a1.65 1.65 0 0 0-1.82-.33 1.65 1.65 0 0 0-1 1.51V21a2 2 0 0 1-2 2 2 2 0 0 1-2-2v-.09A1.65 1.65 0 0 0 9 19.4a1.65 1.65 0 0 0-1.82.33l-.06.06a2 2 0 0 1-2.83 0 2 2 0 0 1 0-2.83l.06-.06a1.65 1.65 0 0 0 .33-1.82 1.65 1.65 0 0 0-1.51-1H3a2 2 0 0 1-2-2 2 2 0 0 1 2-2h.09A1.65 1.65 0 0 0 4.6 9a1.65 1.65 0 0 0-.33-1.82l-.06-.06a2 2 0 0 1 0-2.83 2 2 0 0 1 2.83 0l.06.06a1.65 1.65 0 0 0 1.82.33H9a1.65 1.65 0 0 0 1-1.51V3a2 2 0 0 1 2-2 2 2 0 0 1 2 2v.09a1.65 1.65 0 0 0 1 1.51 1.65 1.65 0 0 0 1.82-.33l.06-.06a2 2 0 0 1 2.83 0 2 2 0 0 1 0 2.83l-.06.06a1.65 1.65 0 0 0-.33 1.82V9a1.65 1.65 0 0 0 1.51 1H21a2 2 0 0 1 2 2 2 2 0 0 1-2 2h-.09a1.65 1.65 0 0 0-1.51 1z"/></svg>
        </span>
        <span>Settings</span>
      </button>
    </nav>
    <div class="sidebar-note">Direct local ESP32 telemetry updates in real-time.</div>
  </aside>

  <main>
    <section class="app-view" id="dashboardView">
      <div class="pagehead">
        <div>
          <div class="eyebrow">Water Level Telemetry</div>
          <h1>Current Water Status</h1>
          <p class="sub">Continuous ultrasonic clearance and tank capacity monitoring. Smaller distance indicates rising water.</p>
        </div>
        <div class="updated" id="updated">Waiting for readings...</div>
      </div>

      <div class="cols">
        <section class="card status-card">
          <div>
            <div class="card-title">WATER LEVEL ASSESSMENT</div>
            <div class="status-title offline" id="mainStatus">Getting readings...</div>
            <p class="status-desc" id="mainDescription">Awaiting fresh sensor telemetry from the monitoring station.</p>
          </div>
          <div class="condition" id="condition">Checking water reader...</div>
        </section>

        <section class="card level-card">
          <div class="level-meta">
            <p class="card-title">SURFACE CLEARANCE TO SENSOR</p>
            <div class="metric">
              <strong id="level">—</strong>
              <span>centimeters</span>
            </div>
            <div class="helper" id="levelNote">The smaller the number, the closer the water is to the device.</div>
            <div class="mini">
              <div>
                <span>Water rise estimate</span>
                <b id="height">—</b>
              </div>
              <div>
                <span>Tank capacity level</span>
                <b id="distance">—</b>
              </div>
            </div>
          </div>
          <div class="home-illustration" role="img" aria-label="Home">
            <svg viewBox="0 0 48 48" aria-hidden="true">
              <path d="M5 21 24 4l19 17M10 19v24h28V19M18 43V29h12v14" fill="none" stroke="currentColor" stroke-linecap="round" stroke-linejoin="round" stroke-width="2.8"/>
              <path d="M14 23h5v5h-5z" fill="currentColor" opacity=".22"/>
            </svg>
          </div>
        </section>
      </div>

      <div class="stats-title">
        <h2>Hardware Diagnostics &amp; Metrics</h2>
        <span>Real-time Telemetry</span>
      </div>

      <div class="sensors">
        <div class="sensor">
          <small>Ultrasonic Sensor</small>
          <strong id="ultraHealth">Checking</strong>
          <div class="secondary">Real-time transducer status</div>
        </div>
        <div class="sensor">
          <small>Water Rise Surge</small>
          <strong id="heightCard">—</strong>
          <div class="secondary">Relative to dry tank baseline</div>
        </div>
        <div class="sensor">
          <small>Clearance Distance</small>
          <strong id="distanceCard">—</strong>
          <div class="secondary">Headroom below sensor</div>
        </div>
        <div class="sensor">
          <small>Threat Classification</small>
          <strong id="alertLevel">—</strong>
          <div class="secondary">Dynamic risk evaluation</div>
        </div>
      </div>
    </section>

    <section class="app-view" id="monitoringView" hidden>
      <div class="pagehead">
        <div>
          <div class="eyebrow">Telemetry Analytics</div>
          <h1>Water Level History</h1>
          <p class="sub">Historical trend telemetry, indicator light states, and hardware switchgear.</p>
        </div>
      </div>

      <div class="lower">
        <section class="card">
          <div class="charthead">
            <b>Water Level Over Time</b>
            <span>Last 60 readings</span>
          </div>
          <canvas id="trend" aria-label="Water level history chart"></canvas>
          <div class="hint" style="margin-top:14px">The chart displays clearance distance below the sensor in centimeters. Buffer holds up to 60 telemetry points.</div>
        </section>

        <section class="card">
          <div class="charthead">
            <b>Hardware Indicators &amp; Annunciator</b>
            <span>Current State</span>
          </div>
          <div class="ledrow">
            <div class="lightbox"><span class="led green" id="ledGreen"></span><b>Water is safe</b></div>
            <div class="lightbox"><span class="led yellow" id="ledYellow"></span><b>Water is rising</b></div>
            <div class="lightbox"><span class="led red" id="ledRed"></span><b>Water is too high</b></div>
          </div>

          <div class="hrow"><span>Power Source indicator</span><strong id="monitorLoadLed">—</strong></div>
          <div class="hrow"><span>Alarm sound</span><strong id="buzzer">—</strong></div>
          <div class="hrow"><span>Alarm sound style</span><strong id="buzzerMode">—</strong></div>
          <div class="hrow"><span>Water change to record</span><strong id="changeAlerts">—</strong></div>
          <div class="hrow"><span>Last water change</span><strong id="changeDelta">—</strong></div>
          <div class="hrow"><span>Changes recorded</span><strong id="changeCount">0</strong></div>
          <div class="hrow"><span>Phones connected</span><strong id="clients">—</strong></div>
          <div class="hrow"><span>System uptime</span><strong id="uptime">—</strong></div>

          <div class="test-panel">
            <div class="field" style="margin-bottom:12px">
              <label for="driverMode">Choose your alarm</label>
              <select id="driverMode" class="driver-select">
                <option value="active">Alarm that makes its own sound</option>
                <option value="passive">Alarm that needs the device to make sound</option>
              </select>
            </div>
            <button class="btn" type="button" id="applyBuzzerModeBtn" style="margin-right:7px">Save choice</button>
            <button class="btn" type="button" id="testBuzzerBtn">Test alarm sound</button>
            <div class="hint">Choose the option that matches your alarm. The alarm sounds when the water is rising or too high. If you hear nothing during the test, check the connections.</div>
            <div id="buzzerTestMessage" class="message" aria-live="polite"></div>
          </div>
        </section>
      </div>
    </section>

    <section class="app-view" id="settingsView" hidden>
      <div class="pagehead">
        <div>
          <div class="eyebrow">System Calibration</div>
          <h1>Settings &amp; Thresholds</h1>
          <p class="sub">Threshold rules and telemetry recording sensitivity configuration.</p>
        </div>
      </div>

      <section class="card appearance-card" aria-labelledby="appearanceTitle">
        <div class="appearance-copy">
          <h2 id="appearanceTitle">Appearance</h2>
          <p>Choose how this dashboard looks on this browser.</p>
        </div>
        <div class="appearance-control">
          <span id="themeToggleLabel">Dark mode (black)</span>
          <button class="theme-toggle" id="themeToggle" type="button" aria-labelledby="themeToggleLabel themeToggleState" aria-pressed="false">
            <span class="theme-toggle-track" aria-hidden="true"></span>
            <span id="themeToggleState">Off</span>
          </button>
        </div>
      </section>

      <div class="cal-head">
        <div>
          <h2>Threshold Rules Matrix</h2>
          <p>Sensor measures air gap clearance below the device. Smaller numbers represent higher water levels.</p>
        </div>
      </div>

      <section class="card">
        <div class="hrow"><span>Water is safe</span><strong>11 centimeters or more below the device</strong></div>
        <div class="hrow"><span>Water is rising — slow beeps</span><strong>7 to under 11 centimeters below the device</strong></div>
        <div class="hrow"><span>Water is too high — fast beeps</span><strong>Less than 7 centimeters below the device</strong></div>
        <div class="hrow"><span>No reading</span><strong>The reader cannot see the water</strong></div>
        <div class="hrow"><span>Power Source indicator when water is safe</span><strong>Turns on by itself, unless turned off after a high-water alert</strong></div>
        <div class="hrow"><span>Power Source indicator when water is rising</span><strong>Keeps its current setting</strong></div>
        <div class="hrow"><span>Power Source indicator when water is too high</span><strong>Turns off by itself</strong></div>
        <div class="hrow"><span>Power Source indicator when there is no reading</span><strong>Keeps its current setting</strong></div>

        <form id="configForm">
          <div class="config-grid">
            <div class="field">
              <label for="changeCm">Smallest water change to record (centimeters)</label>
              <input id="changeCm" name="changeCm" type="number" min="0.5" max="20" step="0.5" required placeholder="1">
            </div>
          </div>
          <div class="settings-bottom">
            <div class="hint">Choose how much the water must move before the change is saved. This does not change when the alarm sounds.</div>
            <button class="btn" id="saveBtn" type="submit">Save setting</button>
          </div>
          <div class="message" id="saveMessage" aria-live="polite"></div>
        </form>
      </section>
    </section>
  </main>
</div>
<footer class="footer">
  <div class="footer-inner">
    <div class="footer-brand">
      <strong>S.E.C.U.R.E APPLICATION</strong>
      <span>Water Level Monitoring &amp; Telemetry System</span>
    </div>
  </div>
</footer>
<script>
const el=id=>document.getElementById(id);
const dashboardThemeKey='secure-dashboard-theme';
function applyDashboardTheme(dark,persist=false){
 document.documentElement.dataset.theme=dark?'dark':'light';
 el('themeToggle').setAttribute('aria-pressed',String(dark));
 el('themeToggleState').textContent=dark?'On':'Off';
 if(persist){
  try{localStorage.setItem(dashboardThemeKey,dark?'dark':'light')}
  catch(error){console.warn('Could not save dashboard appearance preference.',error)}
 }
 const themeColor=document.querySelector('meta[name="theme-color"]');
 if(themeColor)themeColor.content=dark?'#000000':'#0b1329';
 if(persist&&typeof draw==='function')draw();
}
let savedDashboardTheme='light';
try{savedDashboardTheme=localStorage.getItem(dashboardThemeKey)==='dark'?'dark':'light'}
catch(error){console.warn('Could not load dashboard appearance preference.',error)}
applyDashboardTheme(savedDashboardTheme==='dark');
el('themeToggle').addEventListener('click',()=>applyDashboardTheme(el('themeToggle').getAttribute('aria-pressed')!=='true',true));
const buzzerPanel=document.querySelector('.test-panel');
buzzerPanel.querySelector('.field').hidden=true;
buzzerPanel.querySelector('#applyBuzzerModeBtn').hidden=true;
buzzerPanel.querySelector('#testBuzzerBtn').hidden=true;
buzzerPanel.querySelector('.hint').hidden=true;
buzzerPanel.querySelector('#buzzerTestMessage').hidden=true;
el('buzzerMode').closest('.hrow').style.display='none';
const buzzerToggleUi=document.createElement('div');
buzzerToggleUi.innerHTML='<div class="buzzer-toggle-ui"><div class="buzzer-toggle-copy"><strong>Alarm control</strong><span>Enable the buzzer for WARNING and CRITICAL alerts.</span></div><button class="buzzer-toggle-button" id="buzzerToggleBtn" type="button" aria-label="Buzzer enabled" aria-pressed="true"><span class="buzzer-toggle-track" aria-hidden="true"></span><span id="buzzerToggleState">On</span></button></div><div class="buzzer-toggle-message" id="buzzerToggleMessage" role="status" aria-live="polite"></div>';
buzzerPanel.prepend(buzzerToggleUi);
const preview=location.protocol==='file:';
let alertAudioContext=null,alertSoundEnabled=false;
el('menuToggle').addEventListener('click',()=>{
 const collapsed=el('appShell').classList.toggle('sidebar-collapsed');
 const button=el('menuToggle');
 button.setAttribute('aria-expanded',String(!collapsed));
 button.setAttribute('aria-label',collapsed?'Show menu':'Hide menu');
 button.title=collapsed?'Show menu':'Hide menu';
 el('menuToggle').querySelector('.sidebar-toggle-text').textContent=collapsed?'Show menu':'Hide menu';
 document.querySelectorAll('.side-link').forEach(link=>{
  if(collapsed)link.title=link.querySelector('span:last-child').textContent;
  else link.removeAttribute('title');
 });
});
document.querySelectorAll('.side-link').forEach(button=>button.addEventListener('click',()=>{
 const viewId=button.dataset.view;
 document.querySelectorAll('.app-view').forEach(view=>{view.hidden=view.id!==viewId});
 document.querySelectorAll('.side-link').forEach(link=>link.setAttribute('aria-pressed',String(link===button)));
 if(viewId==='monitoringView')draw();
}));
let samples=[],formLoaded=false,polling=false,previewTick=0,previewLedOn=false,lastStatus=null,lastLedState=null,lastBuzzerEnabled=true,toastTimer=0,toastHideTimer=0,notificationsReceived=0,lastNotification=null,notificationHistory=[];
if(preview){el('previewBanner').style.display='block';el('powerSourceToggle').disabled=true;el('powerOffButton').disabled=true;el('powerOnButton').disabled=true;el('saveBtn').disabled=true;el('testBuzzerBtn').disabled=true;el('applyBuzzerModeBtn').disabled=true;el('saveMessage').textContent='Connect to the device to save this setting.';el('buzzerTestMessage').textContent='Connect to the device to test the alarm.';}
if(preview)el('buzzerToggleBtn').disabled=true;
function setLED(id,on){el(id).classList.toggle('on',!!on)}
function playAlertSound(status){
 if(!alertSoundEnabled||!alertAudioContext||alertAudioContext.state!=='running')return false;
 const now=alertAudioContext.currentTime;
 const critical=status==='critical';
 const pulseCount=critical?5:3;
 const pulseLength=critical ? .18 : .14,gap=critical ? .07 : .11;
 const oscillator=alertAudioContext.createOscillator();
 const volume=alertAudioContext.createGain();
 oscillator.type='square';
 volume.gain.setValueAtTime(0,now);
 for(let i=0;i<pulseCount;i++){
  const start=now+i*(pulseLength+gap);
  oscillator.frequency.setValueAtTime(critical?(i%2===0?1450:980):1050,start);
  volume.gain.setValueAtTime(0,start);
  volume.gain.linearRampToValueAtTime(.28,start+.012);
  volume.gain.setValueAtTime(.28,start+pulseLength-.02);
  volume.gain.linearRampToValueAtTime(.001,start+pulseLength);
 }
 oscillator.connect(volume);
 volume.connect(alertAudioContext.destination);
 oscillator.start(now);
 oscillator.stop(now+pulseCount*(pulseLength+gap));
 return true;
}
async function toggleAlertSound(){
 const button=el('alertSoundButton'),message=el('alertSoundStatus');
 if(alertSoundEnabled){
  alertSoundEnabled=false;
  button.classList.remove('sound-on');
  button.setAttribute('aria-pressed','false');
  button.setAttribute('aria-label','Turn on alert sound');
  button.title='Turn on alert sound';
  message.textContent='Alert sound is off.';
  return;
 }
 const AudioContextType=window.AudioContext||window.webkitAudioContext;
 if(!AudioContextType){
  message.textContent='This browser cannot play alert sounds. Try a newer browser.';
  return;
 }
 try{
  if(!alertAudioContext)alertAudioContext=new AudioContextType();
  await alertAudioContext.resume();
  if(alertAudioContext.state!=='running')throw Error('Sound is unavailable');
  alertSoundEnabled=true;
  button.classList.add('sound-on');
  button.setAttribute('aria-pressed','true');
  button.setAttribute('aria-label','Turn off alert sound');
  button.title='Turn off alert sound';
  message.textContent='Alert sound is on. Keep this page open for alerts.';
  playAlertSound('warning');
 }catch(error){
  alertSoundEnabled=false;
  message.textContent='Could not turn on sound. Check your phone sound settings and try again.';
 }
}
el('alertSoundButton').addEventListener('click',toggleAlertSound);
function secs(v){const h=Math.floor(v/3600),m=Math.floor(v%3600/60),s=v%60;return h?h+'h '+m+'m':m?m+'m '+s+'s':s+'s'}
function hideNotification(){clearTimeout(toastTimer);el('statusToast').classList.remove('show');toastHideTimer=setTimeout(()=>{el('statusToast').hidden=true},220)}
function presentNotification(notification,status){
 const toast=el('statusToast');
 clearTimeout(toastTimer);clearTimeout(toastHideTimer);
 const actionable=status==='warning'||status==='critical';
 el('toastDetails').disabled=!actionable;
 el('toastDetails').setAttribute('aria-label',actionable?notification[0]+'. Open alert.':notification[0]);
 toast.className='toast '+status;el('toastTitle').textContent=notification[0];el('toastMessage').textContent=notification[1]+(actionable&&!alertSoundEnabled?' Turn on the sound button to hear alerts.':'');toast.hidden=false;
 if(actionable)playAlertSound(status);
 requestAnimationFrame(()=>toast.classList.add('show'));
 toastTimer=setTimeout(hideNotification,7000);
}
function openPowerDialog(notification=lastNotification){
 if(!notification||(notification.status!=='warning'&&notification.status!=='critical'))return;
 if(lastStatus!=='warning'&&lastStatus!=='critical')return; // historical alerts cannot override current sensor state
 hideNotification();
 const critical=lastStatus==='critical';
 el('powerDialogTitle').textContent=critical?'Water is too high':'Water warning';
 el('powerDialogDescription').textContent=critical
  ?'Water reached '+Number(notification.distance).toFixed(1)+' cm. The Power Source indicator is off.'
  :'Water reached '+Number(notification.distance).toFixed(1)+' cm, close to the danger zone. Turning off the Power Source is recommended.';
 el('powerOffButton').disabled=!lastLedState || preview;
 el('powerOnButton').disabled=preview;
 const dialog=el('powerDialog');
 if(!dialog.open){if(typeof dialog.show==='function')dialog.show(); else dialog.setAttribute('open','');}
}
function closePowerDialog(){
 const dialog=el('powerDialog');
 if(typeof dialog.close==='function'&&dialog.open)dialog.close();
 else dialog.removeAttribute('open');
}
function updateClock(){
 const now=new Date();
 el('clockTime').textContent=now.toLocaleTimeString('en-US', {hour:'numeric',minute:'2-digit',hour12:true});
 el('clockDate').textContent=now.toLocaleDateString([], {weekday:'short',month:'short',day:'numeric',year:'numeric'});
}
function renderNotificationHistory(){
 const list=el('notificationHistoryList');
 list.replaceChildren();
 el('historySummary').textContent=notificationsReceived+' alert'+(notificationsReceived===1?'':'s')+' received';
 if(notificationHistory.length===0){
  const empty=document.createElement('div');empty.className='history-empty';empty.textContent='No alerts yet. New alerts will show here.';list.appendChild(empty);return;
 }
 notificationHistory.forEach(notification=>{
  const item=document.createElement('button');item.type='button';item.className='history-item';
  const actionable=notification.status==='warning'||notification.status==='critical';item.disabled=!actionable;
  const head=document.createElement('span');head.className='history-item-head';
  const title=document.createElement('strong');title.textContent=notification.notice[0];
  const kind=document.createElement('span');kind.className='history-kind '+notification.status;kind.textContent=simpleStatus(notification.status);
  head.append(title,kind);
  const time=document.createElement('time');time.dateTime=notification.receivedAt.toISOString();time.textContent=notification.receivedAt.toLocaleString([], {dateStyle:'medium',timeStyle:'medium'});
  const description=document.createElement('span');description.className='history-item-description';description.textContent=notification.notice[1];
  item.append(head,time,description);
  if(actionable)item.addEventListener('click',()=>{closeNotificationHistory();openPowerDialog(notification)});
  list.appendChild(item);
 });
}
function openNotificationHistory(){
 renderNotificationHistory();
 const dialog=el('notificationHistory');
 if(typeof dialog.show==='function'&&!dialog.open)dialog.show();
 else dialog.setAttribute('open','');
}
function closeNotificationHistory(){
 const dialog=el('notificationHistory');
 if(typeof dialog.close==='function'&&dialog.open)dialog.close();
 else dialog.removeAttribute('open');
}
function simpleStatus(status){
 const labels={safe:'Safe',warning:'Rising',critical:'Too high',fault:'No reading'};
 return labels[status]||'No reading';
}
function notifyStatus(status,distance){
 const previous=lastStatus;lastStatus=status;
 if(status===previous)return;
 if(status==='safe'&&(previous===null||previous==='safe'))return;
 const notices={safe:['Water is safe','The water is lower now. Use Power Source in the header to control the indicator.'],warning:['Water is rising','Water reached '+Number(distance).toFixed(1)+' cm, close to the danger zone. Turning off the Power Source is recommended.'],critical:['Water is too high','The water is '+Number(distance).toFixed(1)+' centimeters below the device. The Power Source indicator is off.'],fault:['No water reading','The device cannot read the water level. The Power Source indicator stays as it is.']};
 const notice=notices[status]||notices.fault,notificationStatus=notices[status]?status:'fault';
 notificationsReceived++;
 el('notificationCount').textContent=notificationsReceived>99?'99+':String(notificationsReceived);
 el('notificationCount').hidden=false;
 el('notificationButton').setAttribute('aria-label','Alerts: '+notificationsReceived);
 lastNotification={notice,status:notificationStatus,distance,receivedAt:new Date()};
 notificationHistory.unshift(lastNotification);
 renderNotificationHistory();
 if(notificationStatus==='warning')playAlertSound(notificationStatus);
 else presentNotification(notice,notificationStatus);
}
el('toastClose').addEventListener('click',hideNotification);
el('toastDetails').addEventListener('click',()=>openPowerDialog());
el('notificationButton').addEventListener('click',openNotificationHistory);
el('historyClose').addEventListener('click',closeNotificationHistory);
// Commands are sent to the ESP32; these controls never merely change a browser icon.
async function sendLedCommand(action){
 if(preview)return;
 const buttons=['powerSourceToggle','powerOffButton','powerOnButton'];
 buttons.forEach(id=>el(id).disabled=true);
 const m=el('powerToggleMessage');m.textContent=action==='on'?'Turning the power source on...':'Turning the power source off...';
 try{
  const controller=new AbortController();const timeout=setTimeout(()=>controller.abort(),4000);
  let response;try{response=await fetch('/api/led',{method:'POST',body:new URLSearchParams({action}),cache:'no-store',signal:controller.signal});}finally{clearTimeout(timeout);}
  const result=await response.json();
  if(!response.ok||!result.ok)throw Error(result.error||'LED command was rejected');
  m.textContent=result.ledOn?'Power Source is on.':'Power Source is off.';
  closePowerDialog();
 }catch(err){m.textContent='Could not change the power source. Check the connection and try again.';}
 finally{await load();}
}
el('powerSourceToggle').addEventListener('click',()=>sendLedCommand(lastLedState?'off':'on'));
el('powerOffButton').addEventListener('click',()=>sendLedCommand('off'));
el('powerOnButton').addEventListener('click',()=>sendLedCommand('on'));
updateClock();setInterval(updateClock,1000);
const offlineBeforeClearingReadings=offline;
offline=function(){
 offlineBeforeClearingReadings();
 samples.length=0;lastStatus=null;lastLedState=null;
 el('powerToggleState').textContent='—';el('powerSourceToggle').setAttribute('aria-pressed','false');
 el('buzzerToggleState').textContent='—';el('buzzerToggleBtn').setAttribute('aria-pressed','false');
 if(el('powerDialog').open)closePowerDialog();
 draw();
};
function offline(){el('linkDot').classList.remove('on');el('linkDot').classList.add('offline');el('linkDot').setAttribute('aria-label','Not connected');el('updated').textContent='Connection lost';el('mainStatus').textContent='No readings';el('mainStatus').className='status-title offline';el('mainDescription').textContent='Connect your phone to the S.E.C.U.R.E network and open 192.168.4.1.';el('condition').textContent='Water level is not available';el('ultraHealth').textContent='No reading';el('heightCard').textContent='—';el('level').textContent='—';el('height').textContent='—';el('distance').textContent='—';el('distanceCard').textContent='—';el('alertLevel').textContent='—';el('buzzer').textContent='—';el('buzzerMode').textContent='—';el('changeAlerts').textContent='—';el('changeDelta').textContent='—';el('changeCount').textContent='—';el('clients').textContent='—';el('uptime').textContent='—';setLED('ledGreen',false);setLED('ledYellow',false);setLED('ledRed',false);el('monitorLoadLed').textContent='—';el('powerSourceToggle').disabled=true;}
function render(d){el('linkDot').classList.add('on');el('linkDot').classList.remove('offline');el('linkDot').setAttribute('aria-label',preview?'Sample readings':'Connected');el('updated').textContent=(preview?'Updated ':'Updated ')+new Date().toLocaleTimeString();
 const previousStatus=lastStatus;
 notifyStatus(d.status,d.distanceCm);
 lastLedState=!!d.ledOn;
 el('monitorLoadLed').textContent=d.ledOn?'ON':'OFF';
 el('powerSourceToggle').disabled=preview;
 el('powerSourceToggle').setAttribute('aria-pressed',String(!!d.ledOn));
 el('powerSourceToggle').setAttribute('aria-label','Power Source is '+(d.ledOn?'on':'off')+'. Turn '+(d.ledOn?'off':'on')+'.');
 el('powerToggleState').textContent=d.ledOn?'On':'Off';
 if(el('powerDialog').open){
   if(d.status==='safe')closePowerDialog();
   else if(d.status==='critical'||d.status==='fault'){
     el('powerDialogTitle').textContent=d.status==='critical'?'Water is too high':'No water reading';
     el('powerDialogDescription').textContent=d.status==='critical'?'Water reached '+Number(d.distanceCm).toFixed(1)+' cm. The Power Source indicator is off.':'The device cannot read the water level. The Power Source indicator stays as it is.';
     el('powerOffButton').disabled=preview||!d.ledOn;el('powerOnButton').disabled=preview;
   }else{
     el('powerDialogTitle').textContent='Water warning';
     el('powerDialogDescription').textContent='Water reached '+Number(d.distanceCm).toFixed(1)+' cm, close to the danger zone. Turning off the Power Source is recommended.';
     el('powerOffButton').disabled=preview||!d.ledOn;el('powerOnButton').disabled=preview;
   }
 }
 // Show the warning each time the reading enters WARNING.
 if(!preview&&d.status==='warning'&&previousStatus!=='warning'){
   openPowerDialog({status:'warning',distance:d.distanceCm,notice:['Water is rising','The water level has gone up.']});
 }
const messages={safe:['Water is safe','The water is 11 centimeters or more below the device.'],warning:['Water is rising','The water is 7 to less than 11 centimeters below the device.'],critical:['Water is too high','The water is less than 7 centimeters below the device.'],fault:['No water reading','Check that the water reader is connected and dry.']};
const [heading,desc]=messages[d.status]||messages.fault;el('mainStatus').textContent=heading;el('mainStatus').className='status-title '+(d.status||'fault');el('mainDescription').textContent=desc;el('condition').textContent=!d.ultrasonicOk?'No water reading':'Water is '+Number(d.distanceCm).toFixed(1)+' centimeters below the device';
el('level').textContent=d.ultrasonicOk?Number(d.distanceCm).toFixed(1):'—';el('levelNote').textContent='Smaller numbers mean the water is closer. Water level is estimated against the tank baseline.';
el('height').textContent=d.ultrasonicOk?Number(d.waterHeightCm).toFixed(1)+' centimeters':'No reading';el('distance').textContent=d.ultrasonicOk?Math.round(d.levelPercent)+'%':'No reading';el('distanceCard').textContent=d.ultrasonicOk?Number(d.distanceCm).toFixed(1)+' centimeters':'No reading';el('ultraHealth').textContent=d.ultrasonicOk?'Working':'No reading';el('heightCard').textContent=d.ultrasonicOk?Number(d.waterHeightCm).toFixed(1)+' centimeters':'No reading';el('alertLevel').textContent=simpleStatus(d.status);
setLED('ledGreen',d.green);setLED('ledYellow',d.yellow);setLED('ledRed',d.red);el('buzzer').textContent=d.buzzer?'On':'Off';el('buzzer').className=d.buzzer?'alarm':'';el('buzzerMode').textContent=d.buzzerType==='passive'?'Needs the device to make sound':'Makes its own sound';el('changeAlerts').textContent=Number(d.changeAlertCm).toFixed(1)+' centimeters';el('changeDelta').textContent=d.lastAlertDeltaCm>=0?Number(d.lastAlertDeltaCm).toFixed(1)+' centimeters · '+(d.lastAlertDirection==='Rising water'?'Water going up':'Water going down'):'Waiting for a change';el('changeCount').textContent=d.distanceChangeAlertCount||0;el('clients').textContent=d.clients;el('uptime').textContent=secs(d.uptime);
if(!formLoaded){el('changeCm').value=d.changeAlertCm;el('driverMode').value=d.buzzerType;formLoaded=true}
samples.push(d.ultrasonicOk?d.distanceCm:null);if(samples.length>60)samples.shift();draw();}
function draw(){
 const c=el('trend');
 if(!c)return;
 const r=c.getBoundingClientRect(),w=Math.max(180,r.width),h=Math.max(100,r.height),scale=window.devicePixelRatio||1;
 c.width=Math.round(w*scale);
 c.height=Math.round(h*scale);
 const x=c.getContext('2d');
 x.scale(scale,scale);
 const left=32,right=w-12,top=12,bottom=h-22;
 const validSamples=samples.filter(v=>v!==null&&Number.isFinite(v));
 const max=Math.max(16,Math.ceil(Math.max(0,...validSamples)/4)*4);

 x.strokeStyle='#e2e8f0';
 x.lineWidth=1;
 x.font='10px ui-monospace, SFMono-Regular, monospace';
 x.fillStyle='#94a3b8';
 for(let t=0;t<=4;t++){
  const y=top+(bottom-top)*t/4;
  x.beginPath();
  x.moveTo(left,y);
  x.lineTo(right,y);
  x.stroke();
  x.fillText(String(max-t*max/4),2,y+3.5);
 }

 const values=samples;
 if(values.length>0){
  const points=[];
  values.forEach((v,i)=>{
   if(v===null||v>max)return;
   const xx=left+(right-left)*i/Math.max(59,values.length-1);
   const yy=bottom-(bottom-top)*v/max;
   points.push({x:xx,y:yy});
  });

  if(points.length>1){
   const area=x.createLinearGradient(0,top,0,bottom);
   area.addColorStop(0,'rgba(37, 99, 235, 0.22)');
   area.addColorStop(1,'rgba(37, 99, 235, 0.0)');

   x.beginPath();
   x.moveTo(points[0].x,points[0].y);
   for(let i=1;i<points.length;i++){
    x.lineTo(points[i].x,points[i].y);
   }
   x.lineTo(points[points.length-1].x,bottom);
   x.lineTo(points[0].x,bottom);
   x.closePath();
   x.fillStyle=area;
   x.fill();

   x.beginPath();
   x.moveTo(points[0].x,points[0].y);
   for(let i=1;i<points.length;i++){
    x.lineTo(points[i].x,points[i].y);
   }
   x.strokeStyle='#2563eb';
   x.lineWidth=2.4;
   x.lineJoin='round';
   x.stroke();

   const lastP=points[points.length-1];
   x.fillStyle='rgba(37, 99, 235, 0.2)';
   x.beginPath();
   x.arc(lastP.x,lastP.y,7,0,2*Math.PI);
   x.fill();

   x.fillStyle='#2563eb';
   x.beginPath();
   x.arc(lastP.x,lastP.y,4,0,2*Math.PI);
   x.fill();

   x.fillStyle='#ffffff';
   x.beginPath();
   x.arc(lastP.x,lastP.y,1.8,0,2*Math.PI);
   x.fill();
  }
 }
}
async function sendBuzzerCommand(action){
 if(preview)return;
 const button=el('buzzerToggleBtn'),message=el('buzzerToggleMessage');
 button.disabled=true;message.className='buzzer-toggle-message';
 message.textContent=action==='on'?'Turning the buzzer on...':'Turning the buzzer off...';
 try{
  const response=await fetch('/api/buzzer/enabled',{method:'POST',body:new URLSearchParams({enabled:action}),cache:'no-store'});
  const result=await response.json();
  if(!response.ok||!result.ok)throw Error(result.error||'Buzzer command was rejected');
  message.textContent=result.buzzerEnabled?'Buzzer enabled.':'Buzzer disabled.';
 }catch(error){
  message.className='buzzer-toggle-message error';
  message.textContent='Could not change buzzer setting. Check the connection and try again.';
 }finally{await load();}
}
el('buzzerToggleBtn').addEventListener('click',()=>sendBuzzerCommand(lastBuzzerEnabled?'off':'on'));
const renderWithBuzzerToggle=render;
render=function(d){
 renderWithBuzzerToggle(d);
 lastBuzzerEnabled=typeof d.buzzerEnabled==='boolean'?d.buzzerEnabled:true;
 el('buzzerToggleBtn').setAttribute('aria-pressed',String(lastBuzzerEnabled));
 el('buzzerToggleBtn').setAttribute('aria-label','Buzzer '+(lastBuzzerEnabled?'enabled':'disabled')+'. Turn '+(lastBuzzerEnabled?'off':'on')+'.');
 el('buzzerToggleState').textContent=lastBuzzerEnabled?'On':'Off';
 el('buzzerToggleBtn').disabled=preview;
};
const offlineWithBuzzerToggle=offline;
offline=function(){offlineWithBuzzerToggle();el('buzzerToggleBtn').disabled=true;};
function simulated(){previewTick++;const cm=Math.max(2,Math.min(15,8.5+Math.sin(previewTick/6)*6));const state=cm<7?'critical':cm<11?'warning':'safe';if(state==='safe')previewLedOn=true;const p=Math.max(0,Math.min(100,(15-cm)*100/15));return {status:state,ledOn:previewLedOn,ultrasonicOk:true,levelPercent:p,distanceCm:cm,waterHeightCm:15-cm,green:state==='safe',yellow:state==='warning',red:state==='critical',buzzer:state==='warning'||state==='critical',buzzerMode:'Active HIGH',buzzerType:'active',changeAlertCm:1,lastAlertDeltaCm:2.1,lastAlertDistanceCm:cm,lastAlertDirection:'Rising water',lastAlertAgeSec:2,distanceChangeAlertCount:4,buzzerTest:false,clients:1,uptime:previewTick};}
async function load(){if(polling)return;polling=true;const ctrl=new AbortController();const timer=setTimeout(()=>ctrl.abort(),3000);try{if(preview){render(simulated());return}const r=await fetch('/api/status?ts='+Date.now(),{cache:'no-store',signal:ctrl.signal});if(!r.ok)throw Error('ESP32 HTTP '+r.status);const d=await r.json();if(!d.ok)throw Error('Invalid sensor response');render(d)}catch(e){offline()}finally{clearTimeout(timer);polling=false}}
el('configForm').addEventListener('submit',async ev=>{ev.preventDefault();if(preview)return;const btn=el('saveBtn'),message=el('saveMessage');btn.disabled=true;message.textContent='Saving...';message.className='message';try{const data=new URLSearchParams(new FormData(ev.target));const r=await fetch('/api/config',{method:'POST',body:data});const j=await r.json();if(!r.ok||!j.ok)throw Error(j.error||'The setting could not be saved');message.textContent='Setting saved.';message.className='message ok';await load()}catch(e){message.textContent='Could not save. Check the connection and try again.';message.className='message error'}finally{btn.disabled=false}});
el('applyBuzzerModeBtn').addEventListener('click',async()=>{
  if(preview)return;
  const btn=el('applyBuzzerModeBtn'),message=el('buzzerTestMessage');btn.disabled=true;
  message.className='message';message.textContent='Saving buzzer choice...';
  try{
    const r=await fetch('/api/buzzer/mode',{method:'POST',body:new URLSearchParams({type:el('driverMode').value})});
    const j=await r.json();if(!r.ok||!j.ok)throw Error(j.error||'The choice could not be saved');
    message.className='message ok';message.textContent='Buzzer choice saved. Test the sound to check it.';
    await load();
  }catch(e){message.className='message error';message.textContent='Could not save the choice. Check the connection and try again.';}finally{btn.disabled=false;}
});
el('testBuzzerBtn').addEventListener('click',async()=>{
  if(preview)return;
  const button=el('testBuzzerBtn'),message=el('buzzerTestMessage');
  button.disabled=true;message.className='message';message.textContent='Testing the alarm...';
  try{
    const ctrl=new AbortController();const timer=setTimeout(()=>ctrl.abort(),4000);
    let response;try{response=await fetch('/api/buzzer/test',{method:'POST',cache:'no-store',signal:ctrl.signal});}finally{clearTimeout(timer);}
    const result=await response.json();
    if(!response.ok||!result.ok)throw Error(result.message||'The alarm could not be tested');
    message.className='message ok';message.textContent='The alarm was tested. If you did not hear it, check the connections.';
    await load();
  }catch(e){message.className='message error';message.textContent='Could not test the alarm. Check the connections and try again.';}
  finally{button.disabled=false;}
});
window.addEventListener('resize',draw);load();setInterval(load,1000);
</script>
</body></html>
)AQHTML";

void setBuzzer(bool on) {
  if (buzzerMode == PASSIVE_PWM) {
    if (buzzerOn != on) {
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
      ledcWriteTone(BUZZER_PIN, on ? BUZZER_FREQUENCY_HZ : 0);
#else
      ledcWriteTone(BUZZER_LEDC_CHANNEL_V2, on ? BUZZER_FREQUENCY_HZ : 0);
#endif
    }
  } else {
    digitalWrite(BUZZER_PIN, (on == (buzzerMode == ACTIVE_HIGH)) ? HIGH : LOW);
  }
  buzzerOn = on;
}

void initializeBuzzer() {
  if (buzzerMode == PASSIVE_PWM) {
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
    if (!ledcAttach(BUZZER_PIN, BUZZER_FREQUENCY_HZ, 8)) {
      Serial.println("ERROR: Could not attach buzzer PWM to GPIO21");
    }
    ledcWriteTone(BUZZER_PIN, 0);
#else
    ledcSetup(BUZZER_LEDC_CHANNEL_V2, BUZZER_FREQUENCY_HZ, 8);
    ledcAttachPin(BUZZER_PIN, BUZZER_LEDC_CHANNEL_V2);
    ledcWriteTone(BUZZER_LEDC_CHANNEL_V2, 0);
#endif
  } else {
    pinMode(BUZZER_PIN, OUTPUT);
    digitalWrite(BUZZER_PIN, buzzerMode == ACTIVE_HIGH ? LOW : HIGH);
  }
  buzzerOn = false;
}

void setBuzzerMode(BuzzerMode next) {
  if (buzzerMode == next) return;
  setBuzzer(false);
  if (buzzerMode == PASSIVE_PWM) {
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
    ledcDetach(BUZZER_PIN);
#else
    ledcDetachPin(BUZZER_PIN);
#endif
  }
  buzzerMode = next;
  initializeBuzzer();
  Serial.printf("GPIO21 buzzer driver switched to: %s\n", buzzerMode == PASSIVE_PWM ? "PASSIVE PWM" : "ACTIVE HIGH");
}

void setLoadLed(bool turnOn) {
  if (loadLedOn == turnOn) return;
  loadLedOn = turnOn;
  if (loadLedOn) {
    loadLedOffTimerActive = false;
  } else {
    loadLedOffSinceMs = millis();
    loadLedOffTimerActive = true;
  }
  digitalWrite(LOAD_LED_PIN, loadLedOn ? HIGH : LOW);
  Serial.printf("GPIO23 LOAD LED: %s\n", loadLedOn ? "ON" : "OFF");
}

void updateStatus() {
  String previous = status;
  if (!ultrasonicOk) {
    status = "fault";
  } else if (distanceCm < WARNING_FROM_CM) {
    status = "critical";  // less than 7 cm, including 6.xx cm
  } else if (distanceCm < SAFE_FROM_CM) {
    status = "warning";   // 7.0 to 10.999 cm
  } else {
    status = "safe";      // 11 cm and farther
  }

  if (status != previous) statusSinceMs = millis();

  // A CRITICAL transition is an automatic, latched shutdown. Returning to
  // SAFE must not re-enable the LED; only a manual ON command clears the latch.
  if (status == "critical" && previous != "critical") {
    loadLedAutoOffLatched = true;
    setLoadLed(false);
    Serial.println("CRITICAL WATER LEVEL: GPIO23 LOAD LED automatically turned OFF and latched");
  }
  // Preserve the original SAFE auto-on behavior only until the first automatic
  // critical shutdown. Once latched, SAFE leaves the LED OFF.
  if (status == "safe" && !loadLedAutoOffLatched) setLoadLed(true);
  if (status != previous && (status == "warning" || status == "critical")) {
    Serial.printf("WATER ALERT: %s at %.1f cm\n", status.c_str(), distanceCm);
  }
}

void updateOutputs() {
  uint32_t now = millis();
  greenOn = status == "safe";
  redOn = status == "critical";
  yellowOn = status == "warning" || (status == "fault" && (now % 1000UL) < 500UL);
  digitalWrite(GREEN_LED_PIN, greenOn ? HIGH : LOW);
  digitalWrite(YELLOW_LED_PIN, yellowOn ? HIGH : LOW);
  digitalWrite(RED_LED_PIN, redOn ? HIGH : LOW);

  const bool manualTest = buzzerTestUntilMs != 0 &&
                          static_cast<int32_t>(buzzerTestUntilMs - now) > 0;
  if (!manualTest) buzzerTestUntilMs = 0;
  const bool warningBeep = status == "warning" &&
                           (now - statusSinceMs) % (WARNING_BUZZER_PULSE_MS * 2) <
                               WARNING_BUZZER_PULSE_MS;
  const bool criticalBeep = status == "critical" &&
                            (now - statusSinceMs) % (CRITICAL_BUZZER_PULSE_MS * 2) <
                                CRITICAL_BUZZER_PULSE_MS;
  const bool sound = buzzerEnabled && (warningBeep || criticalBeep || manualTest);
  setBuzzer(sound);
}

float medianRecentDistances() {
  float sorted[5];
  uint8_t n = distanceCount;
  for (uint8_t i = 0; i < n; i++) sorted[i] = recentDistances[i];
  for (uint8_t i = 1; i < n; i++) {
    float v = sorted[i];
    int j = i - 1;
    while (j >= 0 && sorted[j] > v) { sorted[j + 1] = sorted[j]; j--; }
    sorted[j + 1] = v;
  }
  return n ? sorted[n / 2] : -1.0f;
}

void measureUltrasonic() {
  digitalWrite(ULTRA_TRIG_PIN, LOW);
  delayMicroseconds(3);
  digitalWrite(ULTRA_TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(ULTRA_TRIG_PIN, LOW);
  uint32_t pulseUs = pulseIn(ULTRA_ECHO_PIN, HIGH, ECHO_TIMEOUT_US);
  float observedCm = pulseUs * SOUND_SPEED_CM_PER_US / 2.0f;
  bool valid = pulseUs > 0 && observedCm >= 2.0f && observedCm <= 400.0f;

  if (!valid) {
    if (echoMisses < 255) echoMisses++;
    if (echoMisses >= 3) {
      ultrasonicOk = false;
      distanceCm = -1.0f;
      levelPercent = -1.0f;
      waterHeightCm = -1.0f;
      distanceCount = 0;
      distanceIndex = 0;
      lastChangeReferenceCm = -1.0f;
    }
    return;
  }

  echoMisses = 0;
  recentDistances[distanceIndex] = observedCm;
  distanceIndex = (distanceIndex + 1) % 5;
  if (distanceCount < 5) distanceCount++;
  distanceCm = medianRecentDistances();
  ultrasonicOk = true;
  if (lastChangeReferenceCm < 0.0f) {
    // First valid reading after startup or sensor fault isn't a movement event.
    lastChangeReferenceCm = distanceCm;
  } else {
    const float deltaCm = fabsf(distanceCm - lastChangeReferenceCm);
    const uint32_t now = millis();
    if (deltaCm >= changeAlertCm &&
        (lastChangeAlertMs == 0 || now - lastChangeAlertMs >= CHANGE_ALERT_COOLDOWN_MS)) {
      lastAlertWaterRising = distanceCm < lastChangeReferenceCm;
      lastAlertDeltaCm = deltaCm;
      lastAlertDistanceCm = distanceCm;
      lastAlertAtMs = now;
      lastChangeReferenceCm = distanceCm;
      lastChangeAlertMs = now;
      distanceChangeAlertCount++;
      Serial.printf("DISTANCE CHANGE: %s by %.1f cm -> %.1f cm\n",
                    lastAlertWaterRising ? "water rising" : "water falling", deltaCm, distanceCm);
    }
  }
  // Display only. Does NOT determine the zone/status.
  levelPercent = constrain((DISPLAY_EMPTY_CM - distanceCm) * 100.0f /
                           (DISPLAY_EMPTY_CM - DISPLAY_FULL_CM), 0.0f, 100.0f);
  waterHeightCm = constrain(DISPLAY_EMPTY_CM - distanceCm, 0.0f, DISPLAY_EMPTY_CM);
}

void sampleSensors() {
  measureUltrasonic();
  updateStatus();
}

void sendJson(int code, const String &json) {
  server.sendHeader("Cache-Control", "no-store, max-age=0");
  server.send(code, "application/json; charset=utf-8", json);
}

String buildStatusJson() {
  String json;
  json.reserve(600);
  json += "{\"ok\":true";
  json += ",\"status\":\"" + status + "\"";
  json += ",\"ledOn\":" + String(loadLedOn ? "true" : "false");
  json += ",\"ledCanTurnOn\":true";
  json += ",\"ultrasonicOk\":" + String(ultrasonicOk ? "true" : "false");
  json += ",\"distanceCm\":" + String(distanceCm, 1);
  json += ",\"waterHeightCm\":" + String(waterHeightCm, 1);
  json += ",\"levelPercent\":" + String(levelPercent, 1);
  json += ",\"green\":" + String(greenOn ? "true" : "false");
  json += ",\"yellow\":" + String(yellowOn ? "true" : "false");
  json += ",\"red\":" + String(redOn ? "true" : "false");
  json += ",\"buzzer\":" + String(buzzerOn ? "true" : "false");
  json += ",\"buzzerEnabled\":" + String(buzzerEnabled ? "true" : "false");
  json += ",\"buzzerTest\":" + String(buzzerTestUntilMs != 0 ? "true" : "false");
  json += ",\"buzzerMode\":\"" + String(buzzerMode == PASSIVE_PWM ? "Passive (PWM)" : "Active HIGH") + "\"";
  json += ",\"buzzerType\":\"" + String(buzzerMode == PASSIVE_PWM ? "passive" : "active") + "\"";
  json += ",\"changeAlertCm\":" + String(changeAlertCm, 1);
  json += ",\"lastAlertDeltaCm\":" + String(lastAlertDeltaCm, 1);
  json += ",\"lastAlertDistanceCm\":" + String(lastAlertDistanceCm, 1);
  json += ",\"lastAlertDirection\":\"" + String(lastAlertWaterRising ? "Rising water" : "Falling water") + "\"";
  json += ",\"lastAlertAgeSec\":" + (lastAlertAtMs ? String((millis() - lastAlertAtMs) / 1000UL) : String(-1));
  json += ",\"distanceChangeAlertCount\":" + String(distanceChangeAlertCount);
  json += ",\"clients\":" + String(WiFi.softAPgetStationNum());
  json += ",\"uptime\":" + String(millis() / 1000UL);
  json += ",\"commandAck\":\"" + prefs.getString("commandAck", "") + "\"";
  json += "}";
  return json;
}

void sendStatus() {
  // All fields are generated from ESP32 measurements, not random UI data.
  String json = buildStatusJson();
  sendJson(200, json);
}

String cloudJsonStringField(const String &json, const char *field, int startAt = 0) {
  const String marker = String("\"") + field + "\":\"";
  const int valueStart = json.indexOf(marker, startAt);
  if (valueStart < 0) return "";
  const int contentStart = valueStart + marker.length();
  const int valueEnd = json.indexOf('"', contentStart);
  if (valueEnd < 0) return "";
  return json.substring(contentStart, valueEnd);
}

bool cloudClockIsSynchronized() {
  constexpr time_t MINIMUM_VALID_UNIX_TIME = 1735689600;
  static bool synchronizationRequested = false;

  if (time(nullptr) >= MINIMUM_VALID_UNIX_TIME) return true;
  if (!synchronizationRequested) {
    configTime(0, 0, "pool.ntp.org", "time.google.com");
    synchronizationRequested = true;
    Serial.println("Synchronizing system clock before HTTPS telemetry");
  }

  const uint32_t startedAt = millis();
  while (time(nullptr) < MINIMUM_VALID_UNIX_TIME &&
         millis() - startedAt < 10000UL) {
    vTaskDelay(pdMS_TO_TICKS(250));
  }
  return time(nullptr) >= MINIMUM_VALID_UNIX_TIME;
}

bool cloudJsonFloatField(const String &json, const char *field, float &value, int startAt = 0) {
  const String marker = String("\"") + field + "\":";
  const int valueStart = json.indexOf(marker, startAt);
  if (valueStart < 0) return false;
  const int contentStart = valueStart + marker.length();
  int valueEnd = json.indexOf(',', contentStart);
  const int objectEnd = json.indexOf('}', contentStart);
  if (valueEnd < 0 || (objectEnd >= 0 && objectEnd < valueEnd)) valueEnd = objectEnd;
  if (valueEnd < 0) return false;

  String encoded = json.substring(contentStart, valueEnd);
  encoded.trim();
  char *end = nullptr;
  const float parsed = strtof(encoded.c_str(), &end);
  if (end == encoded.c_str() || *end != '\0' || !isfinite(parsed)) return false;
  value = parsed;
  return true;
}

void applyCloudControlCommand(const String &responseBody) {
  const int commandStart = responseBody.indexOf("\"command\":{");
  if (commandStart < 0) return;

  const String commandId = cloudJsonStringField(responseBody, "id", commandStart);
  const String action = cloudJsonStringField(responseBody, "action", commandStart);
  const String target = cloudJsonStringField(responseBody, "target", commandStart);
  if (commandId.length() != 36) return;
  if (prefs.getString("commandAck", "") == commandId) return;

  if (action == "set-change-alert" && target == "settings") {
    float requested = 0.0f;
    if (!cloudJsonFloatField(responseBody, "value", requested, commandStart) ||
        requested < 0.5f || requested > 20.0f || requested * 2.0f != roundf(requested * 2.0f)) {
      return;
    }
    changeAlertCm = requested;
    lastChangeReferenceCm = -1.0f;
    prefs.putFloat("changeCm", changeAlertCm);
    Serial.printf("Cloud setting saved: log water changes at %.1f cm or greater\n", changeAlertCm);
  } else {
    if ((action != "on" && action != "off") || (target != "led" && target != "buzzer")) return;
    const bool turnOn = action == "on";
    if (target == "buzzer") {
      buzzerEnabled = turnOn;
      prefs.putBool("buzzerEnabled", buzzerEnabled);
      updateOutputs();
    } else {
      loadLedAutoOffLatched = !turnOn;
      setLoadLed(turnOn);
    }
  }
  prefs.putString("commandAck", commandId);
  if (target != "settings") {
    Serial.printf("Cloud control command %s acknowledged: %s %s\n",
                  commandId.c_str(), target == "buzzer" ? "buzzer" : "GPIO23",
                  action == "on" ? "ON" : "OFF");
  }
}

void cloudTelemetryTask(void *parameter) {
  (void)parameter;
  static const char VERCEL_ROOT_CA[] PROGMEM = R"PEM(
-----BEGIN CERTIFICATE-----
MIIFVzCCAz+gAwIBAgINAgPlk28xsBNJiGuiFzANBgkqhkiG9w0BAQwFADBHMQsw
CQYDVQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZpY2VzIExMQzEU
MBIGA1UEAxMLR1RTIFJvb3QgUjEwHhcNMTYwNjIyMDAwMDAwWhcNMzYwNjIyMDAw
MDAwWjBHMQswCQYDVQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZp
Y2VzIExMQzEUMBIGA1UEAxMLR1RTIFJvb3QgUjEwggIiMA0GCSqGSIb3DQEBAQUA
A4ICDwAwggIKAoICAQC2EQKLHuOhd5s73L+UPreVp0A8of2C+X0yBoJx9vaMf/vo
27xqLpeXo4xL+Sv2sfnOhB2x+cWX3u+58qPpvBKJXqeqUqv4IyfLpLGcY9vXmX7w
Cl7raKb0xlpHDU0QM+NOsROjyBhsS+z8CZDfnWQpJSMHobTSPS5g4M/SCYe7zUjw
TcLCeoiKu7rPWRnWr4+wB7CeMfGCwcDfLqZtbBkOtdh+JhpFAz2weaSUKK0Pfybl
qAj+lug8aJRT7oM6iCsVlgmy4HqMLnXWnOunVmSPlk9orj2XwoSPwLxAwAtcvfaH
szVsrBhQf4TgTM2S0yDpM7xSma8ytSmzJSq0SPly4cpk9+aCEI3oncKKiPo4Zor8
Y/kB+Xj9e1x3+naH+uzfsQ55lVe0vSbv1gHR6xYKu44LtcXFilWr06zqkUspzBmk
MiVOKvFlRNACzqrOSbTqn3yDsEB750Orp2yjj32JgfpMpf/VjsPOS+C12LOORc92
wO1AK/1TD7Cn1TsNsYqiA94xrcx36m97PtbfkSIS5r762DL8EGMUUXLeXdYWk70p
aDPvOmbsB4om3xPXV2V4J95eSRQAogB/mqghtqmxlbCluQ0WEdrHbEg8QOB+DVrN
VjzRlwW5y0vtOUucxD/SVRNuJLDWcfr0wbrM7Rv1/oFB2ACYPTrIrnqYNxgFlQID
AQABo0IwQDAOBgNVHQ8BAf8EBAMCAYYwDwYDVR0TAQH/BAUwAwEB/zAdBgNVHQ4E
FgQU5K8rJnEaK0gnhS9SZizv8IkTcT4wDQYJKoZIhvcNAQEMBQADggIBAJ+qQibb
C5u+/x6Wki4+omVKapi6Ist9wTrYggoGxval3sBOh2Z5ofmmWJyq+bXmYOfg6LEe
QkEzCzc9zolwFcq1JKjPa7XSQCGYzyI0zzvFIoTgxQ6KfF2I5DUkzps+GlQebtuy
h6f88/qBVRRiClmpIgUxPoLW7ttXNLwzldMXG+gnoot7TiYaelpkttGsN/H9oPM4
7HLwEXWdyzRSjeZ2axfG34arJ45JK3VmgRAhpuo+9K4l/3wV3s6MJT/KYnAK9y8J
ZgfIPxz88NtFMN9iiMG1D53Dn0reWVlHxYciNuaCp+0KueIHoI17eko8cdLiA6Ef
MgfdG+RCzgwARWGAtQsgWSl4vflVy2PFPEz0tv/bal8xa5meLMFrUKTX5hgUvYU/
Z6tGn6D/Qqc6f1zLXbBwHSs09dR2CQzreExZBfMzQsNhFRAbd03OIozUhfJFfbdT
6u9AWpQKXCBfTkBdYiJ23//OYb2MI3jSNwLgjt7RETeJ9r/tSQdirpLsQBqvFAnZ
0E6yove+7u7Y/9waLd64NnHi/Hm3lCXRSHNboTXns5lndcEZOitHTtNCjv0xyBZm
2tIMPNuzjsmhDYAPexZ3FL//2wmUspO8IFgV6dtxQ/PeEMMA3KgqlbbC1j+Qa3bb
bP6MvPJwNQzcmRk13NfIRmPVNnGuV/u3gm3c
-----END CERTIFICATE-----
)PEM";

  CloudTelemetryRequest request;
  for (;;) {
    if (xQueueReceive(cloudTelemetryQueue, &request, portMAX_DELAY) != pdTRUE) continue;

    const wl_status_t wifiStatus = WiFi.status();
    if (wifiStatus != WL_CONNECTED) {
      Serial.printf("Cloud telemetry skipped: router Wi-Fi status %d\n", static_cast<int>(wifiStatus));
      continue;
    }

    if (!cloudClockIsSynchronized()) {
      Serial.println("ERROR: System clock is not synchronized; HTTPS telemetry skipped");
      continue;
    }

    Serial.printf("Cloud telemetry Wi-Fi connected: IP %s, signal %d dBm\n",
                  WiFi.localIP().toString().c_str(), WiFi.RSSI());

    WiFiClientSecure tlsClient;
    tlsClient.setCACert(VERCEL_ROOT_CA);
    HTTPClient http;
    if (!http.begin(tlsClient, AQUAGUARD_TELEMETRY_URL)) {
      Serial.println("ERROR: Could not start HTTPS telemetry request");
      continue;
    }

    http.setConnectTimeout(10000);
    http.setTimeout(10000);
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Authorization", String("Bearer ") + AQUAGUARD_DEVICE_TOKEN);
    const int responseCode = http.POST(request.json);
    if (responseCode < 200 || responseCode >= 300) {
      const String responseBody = http.getString();
      Serial.printf("ERROR: Cloud telemetry request returned HTTP %d: %s\n",
                    responseCode, responseBody.c_str());
    } else {
      static uint32_t lastSuccessLogMs = 0;
      if (lastSuccessLogMs == 0 || millis() - lastSuccessLogMs >= 30000UL) {
        Serial.printf("Cloud telemetry accepted (HTTP %d)\n", responseCode);
        lastSuccessLogMs = millis();
      }
      const String responseBody = http.getString();
      if (responseBody.indexOf("\"command\":{") >= 0) {
        CloudCommandResponse commandResponse;
        responseBody.toCharArray(commandResponse.body, sizeof(commandResponse.body));
        if (xQueueOverwrite(cloudCommandResponseQueue, &commandResponse) != pdPASS) {
          Serial.println("ERROR: Could not queue cloud control command");
        }
      }
    }
    http.end();
  }
}

void processCloudControlResponses() {
  if (!cloudCommandResponseQueue) return;

  CloudCommandResponse response;
  while (xQueueReceive(cloudCommandResponseQueue, &response, 0) == pdTRUE) {
    applyCloudControlCommand(String(response.body));
  }
}

void sendCloudTelemetry() {
  if (!cloudTelemetryQueue) return;

  const uint32_t now = millis();
  if (now - lastCloudTelemetryMs < CLOUD_TELEMETRY_INTERVAL_MS) return;
  lastCloudTelemetryMs = now;

  const String json = buildStatusJson();
  if (json.length() >= CLOUD_TELEMETRY_JSON_SIZE) {
    Serial.println("ERROR: Cloud telemetry payload exceeds the queue buffer");
    return;
  }

  CloudTelemetryRequest request;
  json.toCharArray(request.json, sizeof(request.json));
  if (xQueueOverwrite(cloudTelemetryQueue, &request) != pdPASS) {
    Serial.println("ERROR: Could not queue cloud telemetry");
  }
}

void postLoadLed() {
  // No GET endpoint for switching; manual control is always available.
  if (!server.hasArg("action")) {
    sendJson(400, "{\"ok\":false,\"error\":\"Missing action. Use on or off.\"}");
    return;
  }
  String action = server.arg("action");
  if (action != "on" && action != "off") {
    sendJson(400, "{\"ok\":false,\"error\":\"Invalid action. Use on or off.\"}");
    return;
  }
  const bool turnOn = action == "on";
  loadLedAutoOffLatched = !turnOn;
  setLoadLed(turnOn);
  sendJson(200, String("{\"ok\":true,\"ledOn\":") + (loadLedOn ? "true" : "false") + "}");
}

void postTestBuzzer() {
  // Test runs for 3 seconds, and automatically returns to normal alarm behavior.
  buzzerTestUntilMs = millis() + 3000UL;
  updateOutputs();
  Serial.println("MANUAL BUZZER TEST: GPIO21 ON for 3 seconds");
  sendJson(200, "{\"ok\":true,\"message\":\"GPIO21 buzzer test started for 3 seconds\"}");
}

void postBuzzerEnabled() {
  if (!server.hasArg("enabled")) {
    sendJson(400, "{\"ok\":false,\"error\":\"Missing enabled setting. Use on or off.\"}");
    return;
  }
  const String enabled = server.arg("enabled");
  if (enabled != "on" && enabled != "off") {
    sendJson(400, "{\"ok\":false,\"error\":\"Invalid enabled setting. Use on or off.\"}");
    return;
  }
  buzzerEnabled = enabled == "on";
  prefs.putBool("buzzerEnabled", buzzerEnabled);
  updateOutputs();
  sendJson(200, String("{\"ok\":true,\"buzzerEnabled\":") +
                    (buzzerEnabled ? "true}" : "false}"));
}

void postBuzzerMode() {
  if (!server.hasArg("type")) { sendJson(400, "{\"ok\":false,\"error\":\"Missing buzzer type.\"}"); return; }
  String type = server.arg("type");
  if (type != "active" && type != "passive") {
    sendJson(400, "{\"ok\":false,\"error\":\"Choose active or passive.\"}"); return;
  }
  BuzzerMode next = type == "passive" ? PASSIVE_PWM : ACTIVE_HIGH;
  setBuzzerMode(next);
  prefs.putUChar("buzmode", (uint8_t)buzzerMode);
  sendJson(200, "{\"ok\":true}");
}

void sendPage() {
  server.sendHeader("Cache-Control", "no-store, max-age=0");
  server.send_P(200, "text/html; charset=utf-8", DASHBOARD_HTML);
}

void postConfig() {
  if (!server.hasArg("changeCm")) {
    sendJson(400, "{\"ok\":false,\"error\":\"Missing movement alert setting.\"}");
    return;
  }
  const float requested = server.arg("changeCm").toFloat();
  if (!isfinite(requested) || requested < 0.5f || requested > 20.0f) {
    sendJson(400, "{\"ok\":false,\"error\":\"Choose 0.5 to 20 cm.\"}");
    return;
  }
  changeAlertCm = requested;
  lastChangeReferenceCm = -1.0f;
  prefs.putFloat("changeCm", changeAlertCm);
  sendJson(200, "{\"ok\":true}");
}

void setup() {
  Serial.begin(115200);
  delay(250);
  loadLedOffSinceMs = millis();
  pinMode(RED_LED_PIN, OUTPUT);
  pinMode(YELLOW_LED_PIN, OUTPUT);
  pinMode(GREEN_LED_PIN, OUTPUT);
  pinMode(LOAD_LED_PIN, OUTPUT);
  digitalWrite(LOAD_LED_PIN, LOW); // initialized low; SAFE turns it on after the first reading
  initializeBuzzer();
  digitalWrite(RED_LED_PIN, LOW);
  digitalWrite(YELLOW_LED_PIN, LOW);
  digitalWrite(GREEN_LED_PIN, LOW);
  pinMode(ULTRA_TRIG_PIN, OUTPUT);
  pinMode(ULTRA_ECHO_PIN, INPUT);
  digitalWrite(ULTRA_TRIG_PIN, LOW);

  prefs.begin("aquaguard", false);
  buzzerEnabled = prefs.getBool("buzzerEnabled", true);
  // The buzzer mode can be changed from the phone without recompiling.
  BuzzerMode storedMode = prefs.getUChar("buzmode", 0) == 2 ? PASSIVE_PWM : ACTIVE_HIGH;
  setBuzzerMode(storedMode);
  changeAlertCm = prefs.getFloat("changeCm", 1.0f);
  if (changeAlertCm < 0.5f || changeAlertCm > 20.0f) changeAlertCm = 1.0f;

  sampleSensors();
  updateOutputs();

  WiFi.mode(WIFI_AP_STA);
  WiFi.softAPConfig(IPAddress(192,168,4,1), IPAddress(192,168,4,1), IPAddress(255,255,255,0));
  if (!WiFi.softAP(AP_SSID, AP_PASSWORD)) Serial.println("ERROR: Cannot create AquaGuard Wi-Fi network");
  if (strlen(AQUAGUARD_WIFI_SSID) > 0) {
    WiFi.onEvent([](arduino_event_id_t event, arduino_event_info_t info) {
      if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
        Serial.printf("Router Wi-Fi disconnected; reason %u\n",
                      static_cast<unsigned>(info.wifi_sta_disconnected.reason));
      }
    });
    WiFi.begin(AQUAGUARD_WIFI_SSID, AQUAGUARD_WIFI_PASSWORD);
    WiFi.setAutoReconnect(true);
    Serial.printf("Connecting cloud telemetry to router Wi-Fi: %s\n", AQUAGUARD_WIFI_SSID);
    if (strlen(AQUAGUARD_DEVICE_TOKEN) >= 32) {
      cloudTelemetryQueue = xQueueCreate(1, sizeof(CloudTelemetryRequest));
      cloudCommandResponseQueue = xQueueCreate(1, sizeof(CloudCommandResponse));
      if (!cloudTelemetryQueue || !cloudCommandResponseQueue) {
        Serial.println("ERROR: Could not allocate cloud telemetry queues");
        if (cloudTelemetryQueue) vQueueDelete(cloudTelemetryQueue);
        if (cloudCommandResponseQueue) vQueueDelete(cloudCommandResponseQueue);
        cloudTelemetryQueue = nullptr;
        cloudCommandResponseQueue = nullptr;
      } else if (xTaskCreate(cloudTelemetryTask, "cloudTelemetry", 8192, nullptr, 1, nullptr) != pdPASS) {
        Serial.println("ERROR: Could not start cloud telemetry task");
        vQueueDelete(cloudTelemetryQueue);
        vQueueDelete(cloudCommandResponseQueue);
        cloudTelemetryQueue = nullptr;
        cloudCommandResponseQueue = nullptr;
      }
    } else {
      Serial.println("Cloud telemetry disabled: configure the device token in secrets.h");
    }
  } else {
    Serial.println("Cloud telemetry disabled: configure router Wi-Fi in secrets.h");
  }
  dns.start(53, "*", WiFi.softAPIP());
  server.on("/", HTTP_GET, sendPage);
  server.on("/api/status", HTTP_GET, sendStatus);
  server.on("/api/led", HTTP_POST, postLoadLed);
  server.on("/api/config", HTTP_POST, postConfig);
  server.on("/api/buzzer/test", HTTP_POST, postTestBuzzer);
  server.on("/api/buzzer/enabled", HTTP_POST, postBuzzerEnabled);
  server.on("/api/buzzer/mode", HTTP_POST, postBuzzerMode);
  server.on("/generate_204", HTTP_GET, sendPage);   // Android portal check
  server.on("/hotspot-detect.html", HTTP_GET, sendPage); // iPhone portal check
  server.on("/fwlink", HTTP_GET, sendPage);
  server.onNotFound(sendPage);
  server.begin();

  Serial.println("\n===== AquaGuard Ultrasonic v2.7 (Safe / Warning / Critical Distance Zones) =====");
  Serial.printf("Buzzer: GPIO21 | mode: %s | test: POST /api/buzzer/test\n",
                buzzerMode == PASSIVE_PWM ? "PASSIVE PWM" : "ACTIVE HIGH");
  Serial.print("Wi-Fi: "); Serial.println(AP_SSID);
  Serial.println("Password: configured in secrets.h");
  Serial.println("Open: http://192.168.4.1");
  Serial.printf("Log movement alert when filtered distance moves by >= %.1f cm\n", changeAlertCm);
  Serial.println("Distance rules: SAFE >= 11 cm; WARNING 7-<11 cm; CRITICAL <7 cm");
  Serial.println("Extra load LED: GPIO23, automatically ON in SAFE; otherwise retains state and is manually controllable");
  Serial.println("HC-SR04 cannot reliably read below approximately 2 cm; no echo = FAULT.");
}

void loop() {
  dns.processNextRequest();
  server.handleClient();
  unsigned long now = millis();
  if (now - lastSampleMs >= SAMPLE_INTERVAL_MS) {
    lastSampleMs = now;
    sampleSensors();
  }
  updateOutputs();
  processCloudControlResponses();
  sendCloudTelemetry();

  static uint32_t lastLogMs = 0;
  if (now - lastLogMs >= 3000UL) {
    lastLogMs = now;
    Serial.printf("Ultrasonic: %s | dist: %.1f cm | water height: %.1f cm | level: %.1f%% | status: %s | buzzer: %s\n",
                  ultrasonicOk ? "OK" : "NO ECHO", distanceCm, waterHeightCm, levelPercent,
                  status.c_str(), buzzerOn ? "ON" : "OFF");
    Serial.printf("Load LED GPIO23: %s\n", loadLedOn ? "ON" : "OFF");
  }
}
