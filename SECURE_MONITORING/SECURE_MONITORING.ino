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
#include <esp_arduino_version.h>

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
#define AQUAGUARD_TELEMETRY_URL "https://aquaguard-flood-dashboard.vercel.app/api/telemetry"
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
constexpr uint32_t WARNING_BUZZER_ON_MS = 500;
constexpr uint32_t WARNING_BUZZER_OFF_MS = 500;
constexpr uint32_t CRITICAL_BUZZER_ON_MS = 300;
constexpr uint32_t CRITICAL_BUZZER_OFF_MS = 300;
constexpr uint32_t BUZZER_POWER_OFF_DELAY_MS = 5000;
constexpr uint32_t CLOUD_TELEMETRY_INTERVAL_MS = 1000;

constexpr uint32_t SAMPLE_INTERVAL_MS = 420;
constexpr uint32_t ECHO_TIMEOUT_US = 27000;
constexpr float SOUND_SPEED_CM_PER_US = 0.0343f;

WebServer server(80);
DNSServer dns;
Preferences prefs;

bool ultrasonicOk = false;
float distanceCm = -1.0f;
float levelPercent = -1.0f;
float waterHeightCm = -1.0f;
uint8_t echoMisses = 0;
float recentDistances[5] = {0};
uint8_t distanceCount = 0, distanceIndex = 0;

String status = "fault"; // safe / warning / critical / fault
bool buzzerOn = false;
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
float lastChangeReferenceCm = -1.0f;
uint32_t lastChangeAlertMs = 0;
uint32_t distanceChangeAlertCount = 0;
uint32_t statusSinceMs = 0;
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
<meta name="theme-color" content="#10213b">
<title>AquaGuard | Water Level</title>
<style>
:root{--ink:#18243b;--muted:#728197;--navy:#10213b;--blue:#2867ee;--line:#e7edf4;--paper:#f3f6fa;--safe:#12986d;--warn:#b77a0e;--danger:#cf4654;--radius:18px}
*{box-sizing:border-box}html{scroll-behavior:smooth}body{font-family:Inter,'Segoe UI',Arial,sans-serif;background:var(--paper);color:var(--ink);margin:0;font-size:14px}button,input{font:inherit}button{cursor:pointer}button:disabled{cursor:wait;opacity:.6}
header{background:#0e213d;color:white}.nav{max-width:1160px;margin:auto;min-height:82px;display:flex;align-items:center;justify-content:space-between;padding:17px 25px;gap:15px}.brand{display:flex;align-items:center;gap:11px}.logo{width:43px;height:43px;border-radius:13px;background:#2c68e0;display:grid;place-items:center;font-size:25px;color:#fff}.brand strong{font-size:20px;letter-spacing:-.6px;display:block}.brand small{color:#9db6dc;letter-spacing:1.7px;font-size:10px;font-weight:750}.net{padding:9px 13px;border:1px solid #39516c;background:#1a3152;border-radius:50px;color:#d2e3fb;font-size:12px;display:flex;align-items:center;gap:9px}.dot{width:9px;height:9px;background:#dca54f;display:inline-block;border-radius:50%}.dot.on{background:#4cddaf;box-shadow:0 0 0 4px #4cddaf24}
header{background:#0e213d;color:white}.nav{max-width:1160px;margin:auto;min-height:82px;display:flex;align-items:center;justify-content:space-between;padding:17px 25px;gap:15px}.brand{display:flex;align-items:center;gap:11px}.logo{width:43px;height:43px;border-radius:13px;background:#2c68e0;display:grid;place-items:center;font-size:25px;color:#fff}.brand strong{font-size:20px;letter-spacing:-.6px;display:block}.brand small{color:#9db6dc;letter-spacing:1.7px;font-size:10px;font-weight:750}.header-tools{display:flex;align-items:center;gap:12px}.clock{min-width:122px;text-align:right}.clock strong{display:block;color:#f3f7ff;font-size:15px;font-variant-numeric:tabular-nums;letter-spacing:.4px}.clock small{display:block;color:#9db6dc;font-size:10px;margin-top:3px}.notification-button{position:relative;width:42px;height:42px;display:grid;place-items:center;border:1px solid #39516c;border-radius:12px;background:#1a3152;color:#d2e3fb}.notification-button:hover{background:#254368}.notification-button svg{width:20px;height:20px}.notification-count{position:absolute;top:-6px;right:-6px;min-width:19px;height:19px;padding:0 5px;display:grid;place-items:center;border:2px solid #0e213d;border-radius:20px;background:#ed5360;color:white;font-size:10px;font-weight:800;line-height:1}.notification-count[hidden]{display:none}.net{padding:9px 13px;border:1px solid #39516c;background:#1a3152;border-radius:50px;color:#d2e3fb;font-size:12px;display:flex;align-items:center;gap:9px}.dot{width:9px;height:9px;background:#dca54f;display:inline-block;border-radius:50%}.dot.on{background:#4cddaf;box-shadow:0 0 0 4px #4cddaf24}
main{min-width:0;max-width:none;margin:0;padding:29px clamp(18px,3vw,42px) 36px}.app-shell{max-width:1500px;min-height:calc(100vh - 82px);margin:0 auto;display:grid;grid-template-columns:246px minmax(0,1fr);background:var(--paper)}.sidebar{background:#fff;border-right:1px solid var(--line);padding:26px 16px;display:flex;flex-direction:column;gap:9px}.sidebar-label{padding:0 12px 10px;color:#94a1b3;font-size:10px;font-weight:800;letter-spacing:1.5px}.side-link{width:100%;display:flex;align-items:center;gap:12px;border:0;border-radius:11px;padding:13px 12px;text-align:left;background:transparent;color:#607087;font-size:14px;font-weight:700;transition:background .15s,color .15s}.side-link:hover{background:#f1f5fb;color:#213b60}.side-link[aria-pressed="true"]{background:#eaf1ff;color:#245fc9}.side-icon{width:24px;text-align:center;font-size:17px}.sidebar-note{margin-top:auto;padding:14px 12px;border-top:1px solid var(--line);color:#8290a4;font-size:11px;line-height:1.6}.app-view[hidden]{display:none}.eyebrow{color:#617ea7;font-size:11px;font-weight:800;letter-spacing:1.7px;text-transform:uppercase}h1{font-size:clamp(27px,4vw,36px);letter-spacing:-1.2px;margin:10px 0 8px}.sub{color:var(--muted);margin:0;line-height:1.6}.pagehead{display:flex;align-items:flex-end;justify-content:space-between;gap:16px;flex-wrap:wrap;margin-bottom:24px}.updated{color:#74849c;font-size:12px;background:#e7edf7;padding:10px 13px;border-radius:30px}
.card{background:#fff;border:1px solid var(--line);border-radius:var(--radius);padding:23px;box-shadow:0 12px 28px #203a6609;min-width:0}.cols{display:grid;grid-template-columns:1.12fr .88fr;gap:17px}.card-title{color:#7d8a9d;font-size:12px;text-transform:uppercase;letter-spacing:1px;font-weight:800;margin:0}.status-card{min-height:294px;background:linear-gradient(135deg,#112d55,#0d2141 75%);color:white;border:none;display:flex;flex-direction:column;justify-content:space-between;overflow:hidden;position:relative}.status-card:after{content:'';width:260px;height:260px;position:absolute;border-radius:50%;background:rgba(74,130,234,.13);right:-115px;top:-115px}.status-card .card-title{color:#a8bde3}.status-title{margin:24px 0 12px;font-weight:850;font-size:clamp(27px,4vw,39px);line-height:1.1;letter-spacing:-1.1px;position:relative;z-index:1}.status-title.safe{color:#77e6ba}.status-title.warning{color:#ffd178}.status-title.critical{color:#ff9dab}.status-title.fault{color:#f2c276}.status-title.offline{color:#e1e8f2}.status-desc{line-height:1.7;max-width:440px;color:#c7d7f0;font-size:13px;margin:0;position:relative;z-index:1}.condition{margin-top:22px;font-size:12px;display:inline-block;color:#cbdcf8;border:1px solid #476289;background:#23416866;padding:10px 13px;border-radius:10px;align-self:flex-start}
.level-card{display:flex;align-items:center;justify-content:space-between;gap:15px;min-height:294px}.metric{margin:15px 0 4px;display:flex;align-items:baseline;gap:5px;flex-wrap:wrap}.metric strong{font-size:clamp(38px,5vw,56px);letter-spacing:-2px}.metric span{color:#8390a1;font-size:15px}.helper{color:#7c899c;font-size:12px;line-height:1.55;max-width:310px}.mini{margin-top:17px;color:#647c97;display:grid;gap:8px;font-size:12px}.mini div{display:flex;gap:12px;justify-content:space-between}.mini b{color:#30415a}.tank{height:150px;width:150px;flex:none;clip-path:polygon(50% 0,100% 42%,86% 42%,86% 100%,14% 100%,14% 42%,0 42%);position:relative;overflow:hidden;background:#42b9e8;filter:drop-shadow(0 3px 4px #1c568633)}.tank:before{content:"";position:absolute;inset:6px;background:linear-gradient(135deg,#f6fbff,#e9f4ff);clip-path:polygon(50% 2%,96% 43%,84% 43%,84% 96%,16% 96%,16% 43%,4% 43%);z-index:0}.tank:after{content:"";position:absolute;left:8px;right:8px;top:8px;bottom:8px;background:repeating-linear-gradient(to bottom,transparent 0,transparent 38px,#bdd0e480 39px,#bdd0e480 40px);clip-path:polygon(50% 2%,96% 43%,84% 43%,84% 96%,16% 96%,16% 43%,4% 43%);pointer-events:none;z-index:2}.tank-water{position:absolute;bottom:8px;left:8px;right:8px;height:0%;background:linear-gradient(#6bd1ed,#307dec);clip-path:polygon(50% 2%,96% 43%,84% 43%,84% 96%,16% 96%,16% 43%,4% 43%);transition:height .65s ease;min-height:0;z-index:1}.tank-water:before{content:"";position:absolute;height:9px;background:#adeaf9c0;top:0;left:0;right:0}.tank-text{position:absolute;inset:0;display:grid;place-items:center;z-index:3;font-weight:800;color:#193d6d;text-shadow:0 1px 6px white;font-size:15px;padding-top:34px}
.stats-title{display:flex;justify-content:space-between;align-items:center;gap:12px;margin:26px 0 13px}.stats-title h2{font-size:17px;letter-spacing:-.3px;margin:0}.stats-title span{color:#95a1b2;font-size:12px}.sensors{display:grid;grid-template-columns:repeat(4,1fr);gap:13px}.sensor{border:1px solid var(--line);border-radius:14px;background:#fff;padding:18px;min-width:0}.sensor small{display:block;color:#8592a6;font-size:12px;margin-bottom:8px}.sensor strong{font-size:18px;letter-spacing:-.4px;overflow-wrap:anywhere}.sensor .secondary{color:#8290a5;font-size:11px;margin-top:8px}
.lower{display:grid;grid-template-columns:1.2fr .8fr;gap:16px;margin-top:17px}.charthead{display:flex;align-items:center;justify-content:space-between;gap:8px;margin-bottom:18px}.charthead b{font-size:15px}.charthead span{color:#93a1b0;font-size:11px}canvas{width:100%;height:182px;display:block}.ledrow{display:grid;grid-template-columns:repeat(3,1fr);gap:10px;margin:16px 0 20px}.lightbox{border-radius:13px;background:#f7f9fc;padding:13px 8px;text-align:center;border:1px solid #edf1f6}.led{display:block;width:29px;height:29px;border-radius:50%;background:#d8dee7;margin:2px auto 12px;box-shadow:inset 0 0 0 5px #edf1f6}.led.green.on{background:#16c28b;box-shadow:0 0 0 4px #dcf9ed,0 0 15px #28c691}.led.yellow.on{background:#f0b83b;box-shadow:0 0 0 4px #fff1d2,0 0 15px #e9bd59}.led.red.on{background:#ee5461;box-shadow:0 0 0 4px #ffdde2,0 0 15px #f17884}.lightbox b{font-size:12px}.lightbox small{display:block;margin-top:3px;color:#95a0b0;font-size:10px}.hrow{display:flex;align-items:center;justify-content:space-between;padding:11px 0;border-bottom:1px solid #eef2f7;font-size:12px;gap:8px}.hrow:last-child{border:0}.hrow span{color:#78869b}.hrow strong{color:#273a57}.hrow strong.alarm{color:var(--danger)}.test-panel{border-top:1px solid #e7edf4;margin-top:14px;padding-top:15px}.test-panel .btn{width:100%;background:#173b70}.test-panel .btn:hover{background:#102947}.test-panel .hint{margin-top:9px;font-size:11px}.test-panel .message{min-height:0}.test-panel .message.ok{color:#0b926b}
.cal-head{display:flex;justify-content:space-between;align-items:flex-start;flex-wrap:wrap;gap:12px;margin-top:26px;margin-bottom:12px}.cal-head h2{margin:0;font-size:17px}.cal-head p{margin:5px 0 0;color:#8491a4;line-height:1.6;font-size:12px}.config-grid{display:grid;grid-template-columns:repeat(4,minmax(0,1fr));gap:12px}.field label{display:block;color:#566982;font-weight:700;font-size:12px;margin-bottom:8px}.field input{border:1px solid #dce4ef;border-radius:10px;padding:12px 11px;width:100%;color:#172942;background:#fff;font-size:14px;outline:none}.field input:focus{border-color:#4d83e8;box-shadow:0 0 0 3px #377aff1b}.settings-bottom{display:flex;align-items:center;justify-content:space-between;gap:12px;flex-wrap:wrap;margin-top:17px}.hint{color:#78889f;font-size:12px;line-height:1.7;max-width:710px}.btn{border:0;background:#2768e9;color:white;padding:12px 18px;border-radius:11px;font-size:13px;font-weight:800;white-space:nowrap}.btn:hover{background:#1d54c0}.message{min-height:20px;margin-top:10px;font-size:12px;color:#526880}.message.error{color:#d24753}.message.ok{color:#0b926b}
.footer{text-align:center;color:#93a0b0;font-size:11px;line-height:1.7;padding:0 20px 30px}.preview{background:#fff3d5;color:#775621;font-size:12px;padding:10px 15px;text-align:center}
@media(max-width:900px){.sensors{grid-template-columns:repeat(2,1fr)}.config-grid{grid-template-columns:repeat(2,minmax(0,1fr))}}
@media(max-width:680px){.nav{padding:14px 16px}.brand strong{font-size:19px}.header-tools{gap:8px}.clock{min-width:0}.clock strong{font-size:13px}.clock small{font-size:9px}.notification-button{width:38px;height:38px}.net{padding:8px 10px;font-size:11px}.app-shell{grid-template-columns:1fr;min-height:0}.sidebar{position:sticky;top:0;z-index:10;border-right:0;border-bottom:1px solid var(--line);padding:10px 12px;display:block}.sidebar-label,.sidebar-note{display:none}.side-nav{display:flex;gap:7px}.side-link{justify-content:center;gap:7px;padding:10px 5px;font-size:12px}.side-icon{width:auto}.side-link span:last-child{white-space:nowrap}main{padding:24px 14px 28px}.cols,.lower{grid-template-columns:1fr}.card{padding:19px}.level-card{min-height:255px}.tank{height:132px;width:132px}.config-grid{grid-template-columns:repeat(2,minmax(0,1fr))}.status-card{min-height:250px}.btn{width:100%}.settings-bottom{align-items:stretch}.sensors{gap:10px}.sensor{padding:15px}}
@media(max-width:345px){.config-grid{grid-template-columns:1fr}.net{font-size:10px}.tank{height:116px;width:116px}.metric strong{font-size:36px}.sensors{grid-template-columns:1fr}}
select.driver-select{width:100%;border:1px solid #dae3ed;border-radius:10px;padding:12px;background:white;color:#243450;margin-top:6px}.test-panel .btn{margin-bottom:8px}
.notification-slot{display:contents}.toast{position:fixed;top:96px;right:18px;z-index:1000;width:min(380px,calc(100vw - 36px));margin:0;display:flex;align-items:flex-start;gap:12px;padding:12px 14px;background:#fff;border:1px solid #e5eaf1;border-left:5px solid #2867ee;border-radius:14px;box-shadow:0 16px 42px #12284930;opacity:0;transform:translateY(-8px);transition:opacity .2s ease,transform .2s ease}.toast.show{opacity:1;transform:translateY(0)}.toast.warning{border-left-color:#d99a21}.toast.critical{border-left-color:#cf4654}.toast.fault{border-left-color:#b77a0e}.toast.safe{border-left-color:#12986d}.toast-icon{width:32px;height:32px;flex:none;border-radius:50%;display:grid;place-items:center;background:#edf3ff;color:#2867ee;font-weight:900}.toast.warning .toast-icon{background:#fff5df;color:#a86f05}.toast.critical .toast-icon{background:#fff0f2;color:#cf4654}.toast.fault .toast-icon{background:#fff5df;color:#a86f05}.toast.safe .toast-icon{background:#e8f9f2;color:#12986d}.toast-main{flex:1;min-width:0;display:flex;align-items:flex-start;gap:12px;padding:2px 0;border:0;background:transparent;text-align:left;color:inherit}.toast-main:not(:disabled){cursor:pointer}.toast-main:not(:disabled):hover .toast-copy strong{text-decoration:underline}.toast-main:disabled{cursor:default;opacity:1}.toast-copy{flex:1;min-width:0}.toast-copy strong{display:block;color:#18243b;font-size:14px;margin:1px 0 4px}.toast-copy span{display:block;color:#68788e;font-size:12px;line-height:1.5}.toast-close{border:0;background:transparent;color:#8794a6;padding:0 2px;font-size:20px;line-height:1}.toast[hidden]{display:none}
.power-dialog{position:fixed;z-index:2000;inset:96px 18px auto auto;margin:0;width:min(390px,calc(100vw - 36px));max-height:calc(100vh - 112px);overflow-y:auto;border:0;border-radius:18px;padding:19px;color:var(--ink);box-shadow:0 16px 42px #12284930}.power-dialog::backdrop{background:transparent}.power-dialog h2{margin:0 0 8px;font-size:19px}.power-dialog p{color:#65758b;font-size:13px;line-height:1.5;margin:0}.power-actions{display:grid;grid-template-columns:1fr 1fr;gap:9px;margin-top:15px}.power-action{border:0;border-radius:10px;padding:11px 10px;background:#dce2eb;color:#667287;font-size:13px;font-weight:800}.power-action:disabled{cursor:not-allowed;opacity:.65}.power-action.action-off{background:#dc3e50;color:white}.power-action.action-on{background:#e6f7ec;color:#177346}
.history-dialog{position:fixed;z-index:2000;inset:96px 18px auto auto;margin:0;width:min(540px,calc(100vw - 36px));max-height:calc(100vh - 112px);border:0;border-radius:18px;padding:0;color:var(--ink);box-shadow:0 16px 42px #12284930;overflow:hidden}.history-dialog::backdrop{background:transparent}.history-head{display:flex;align-items:flex-start;justify-content:space-between;gap:15px;padding:21px 23px 16px;border-bottom:1px solid var(--line)}.history-head h2{margin:0;font-size:20px}.history-head p{margin:5px 0 0;color:#8491a4;font-size:12px}.history-close{width:34px;height:34px;border:0;border-radius:9px;background:#f1f4f8;color:#53647b;font-size:21px;line-height:1}.history-list{max-height:calc(100vh - 220px);overflow-y:auto;padding:8px 14px 14px}.history-empty{padding:28px 14px;text-align:center;color:#8390a3;font-size:13px}.history-item{width:100%;display:block;padding:13px 10px;text-align:left;border:0;border-bottom:1px solid #edf1f6;background:#fff;color:inherit}.history-item:last-child{border-bottom:0}.history-item:not(:disabled){cursor:pointer}.history-item:not(:disabled):hover{background:#f7f9fc}.history-item:disabled{cursor:default;opacity:1}.history-item-head{display:flex;align-items:center;justify-content:space-between;gap:10px}.history-item-head strong{font-size:13px}.history-kind{padding:4px 8px;border-radius:20px;background:#f1f4f8;color:#718096;font-size:10px;font-weight:800;text-transform:uppercase}.history-kind.warning{background:#fff5df;color:#a86f05}.history-kind.critical{background:#fff0f2;color:#c83e4c}.history-kind.fault{background:#fff5df;color:#a86f05}.history-kind.safe{background:#e8f9f2;color:#12845f}.history-item time{display:block;margin-top:5px;color:#8592a5;font-size:11px;font-variant-numeric:tabular-nums}.history-item-description{display:block;margin-top:5px;color:#68788e;font-size:12px;line-height:1.5}
@media(max-width:680px){.toast{top:82px;right:12px;width:calc(100vw - 24px)}.power-dialog,.history-dialog{inset:82px 12px auto auto;width:calc(100vw - 24px);max-height:calc(100vh - 94px)}.history-list{max-height:calc(100vh - 200px)}}
/* Power Source page controls for the additional low-voltage LED. */
.led-control-card{margin-top:17px}.load-led-layout{display:flex;align-items:center;justify-content:space-between;gap:22px;flex-wrap:wrap}.load-led-status{display:flex;align-items:center;gap:14px}.load-led-dot{height:16px;width:16px;flex:none;border-radius:50%;background:#a5acb9;box-shadow:0 0 0 5px #f0f3f7}.load-led-dot.on{background:#20a574;box-shadow:0 0 0 5px #e1f6ed}.load-led-status strong{font-size:20px;letter-spacing:-.4px}.load-led-actions{display:flex;gap:11px;flex-wrap:wrap}.load-led-actions button{min-width:132px;border:1px solid transparent;border-radius:11px;padding:13px 20px;font-size:14px;font-weight:750;transition:background .15s,box-shadow .15s,transform .15s}.load-led-actions button:not(:disabled):hover{transform:translateY(-1px);box-shadow:0 5px 13px #1228491c}.load-led-actions button:focus-visible{outline:3px solid #8bb5ff;outline-offset:3px}.load-led-actions button:disabled{opacity:.5;cursor:not-allowed}.load-led-on{background:#137c5b;color:white}.load-led-on:not(:disabled):hover{background:#0e684c}.load-led-off{background:#fff;color:#b43e4b;border-color:#e5bfc3!important}.load-led-off:not(:disabled):hover{background:#fff3f4}.load-led-help{color:#718096;font-size:13px;line-height:1.7;margin:20px 0 0;padding-top:16px;border-top:1px solid var(--line)}.led-control-message{min-height:20px;margin-top:12px;font-size:13px;color:#42617b}
@media(max-width:680px){.load-led-layout{align-items:flex-start}.load-led-actions{width:100%}.load-led-actions button{flex:1}}
/* Keep the dashboard comfortable on tablets and narrow phone screens. */
@media(max-width:900px){
 .app-shell{grid-template-columns:210px minmax(0,1fr)}
 main{padding:24px 20px 30px}
 .cols,.lower{grid-template-columns:1fr}
 .status-card,.level-card{min-height:245px}
}
@media(max-width:680px){
 html,body{width:100%;min-width:0;overflow-x:hidden}
 .nav{min-height:0;flex-wrap:wrap;padding:12px 14px;gap:10px}
 .header-tools{width:100%;justify-content:space-between;flex-wrap:wrap}
 .connection-clock{min-width:0;gap:4px}
 .connection-clock .clock{text-align:center}
 .app-shell{display:block;min-height:0}
 .sidebar{position:sticky;top:0;padding:8px;background:#fff}
 .side-nav{display:grid;grid-template-columns:repeat(3,minmax(0,1fr));gap:5px}
 .side-link{min-width:0;min-height:48px;flex-direction:column;justify-content:center;gap:2px;padding:5px 2px;font-size:10px;line-height:1.15}
 .side-link span:last-child{white-space:normal;text-align:center;overflow-wrap:anywhere}
 .side-icon{font-size:16px;line-height:1}
 main{padding:20px 13px 26px}
 .pagehead{align-items:flex-start;gap:10px;margin-bottom:17px}
 .pagehead>div{min-width:0}
 .updated{max-width:100%;padding:8px 10px;font-size:11px}
 .cols,.lower{grid-template-columns:minmax(0,1fr);gap:12px}
 .card{padding:17px;border-radius:15px}
 .status-card{min-height:220px}
 .status-title{font-size:clamp(26px,8vw,36px);margin:18px 0 10px}
 .condition{margin-top:16px;max-width:100%;line-height:1.5}
 .level-card{min-height:210px;gap:8px}
 .tank{width:clamp(100px,32vw,132px);height:clamp(100px,32vw,132px)}
 .metric strong{font-size:clamp(38px,12vw,52px)}
 .metric span{font-size:13px}
 .sensors{grid-template-columns:repeat(2,minmax(0,1fr));gap:9px}
 .sensor{padding:13px}
 .sensor strong{font-size:16px}
 .stats-title{align-items:flex-start;flex-wrap:wrap;margin-top:20px}
 .config-grid{grid-template-columns:minmax(0,1fr)}
 .settings-bottom{align-items:stretch}
 .settings-bottom .btn,.test-panel .btn{width:100%;min-height:44px;white-space:normal}
 .load-led-actions{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:8px}
 .load-led-actions button{min-width:0;min-height:46px;padding:11px 8px}
 .power-actions{grid-template-columns:1fr}
 .power-action,.dialog-close,.history-close{min-height:44px}
 .history-dialog{overflow:hidden}
 .history-head{padding:17px}
 .history-list{max-height:calc(100dvh - 190px)}
 canvas{height:170px}
 .footer{padding-right:14px;padding-left:14px}
}
@media(max-width:400px){
 .nav{padding:10px 12px}
 .header-tools{gap:6px}
 .clock strong{font-size:12px}
 .clock small{font-size:9px}
 .net{padding:7px 8px;font-size:10px}
 main{padding-right:10px;padding-left:10px}
 .card{padding:14px}
 .level-card{align-items:flex-start}
 .tank{width:100px;height:100px;margin-top:18px}
 .mini div{gap:6px;font-size:11px}
 .sensor{padding:11px}
 .lightbox{padding:11px 4px}
 .lightbox b{font-size:10px}
 .hrow{font-size:11px}
 .hrow strong{text-align:right}
 .dialog-distance{align-items:flex-start;gap:8px;flex-direction:column}
 .toast{gap:8px;padding:10px}
 .toast-main{gap:8px}
}
@media(prefers-reduced-motion:reduce){
 html{scroll-behavior:auto}
 *,*::before,*::after{scroll-behavior:auto!important;animation-duration:.01ms!important;animation-iteration-count:1!important;transition-duration:.01ms!important}
}
.sidebar-head{display:flex;align-items:center;justify-content:space-between;gap:8px}.sidebar-head .sidebar-label{padding-bottom:0}.sidebar-menu-toggle{width:42px;height:42px;display:grid;place-items:center;border:1px solid var(--line);border-radius:11px;background:#fff;color:#52647c}.sidebar-menu-toggle:hover{background:#f1f5fb;color:#245fc9}.sidebar-menu-toggle svg{width:19px;height:19px}.sidebar-toggle-text,.sr-only{position:absolute;width:1px;height:1px;padding:0;margin:-1px;overflow:hidden;clip:rect(0,0,0,0);white-space:nowrap;border:0}
.app-shell{transition:grid-template-columns .2s ease}.app-shell.sidebar-collapsed{grid-template-columns:76px minmax(0,1fr)}.sidebar-collapsed .sidebar{align-items:center;padding-right:10px;padding-left:10px}.sidebar-collapsed .sidebar-head{width:100%;justify-content:center}.sidebar-collapsed .sidebar-label,.sidebar-collapsed .sidebar-note,.sidebar-collapsed .side-link span:last-child{display:none}.sidebar-collapsed .side-link{justify-content:center;padding-right:5px;padding-left:5px}.sidebar-collapsed .side-icon{width:auto}
.connection-clock{display:flex;flex-direction:column;align-items:center;gap:6px;min-width:122px}.connection-clock .clock{text-align:center}.dot.on{animation:connection-pulse 1.4s ease-in-out infinite}.dot.offline{background:#e16a73;box-shadow:0 0 0 4px #e16a7324}@keyframes connection-pulse{0%,100%{box-shadow:0 0 0 3px #4cddaf28}50%{box-shadow:0 0 0 7px #4cddaf08}}
.power-source-toggle{display:flex;align-items:center;gap:9px;min-height:42px;padding:7px 11px;border:1px solid #39516c;border-radius:12px;background:#1a3152;color:#d2e3fb;font-size:12px;font-weight:700;white-space:nowrap}.power-source-toggle:hover:not(:disabled){background:#254368}.power-source-toggle:focus-visible{outline:3px solid #8bb5ff;outline-offset:2px}.power-source-toggle[aria-pressed="true"]{border-color:#3a967b}.power-switch-track{width:34px;height:20px;position:relative;flex:none;border-radius:20px;background:#65748b;transition:background .18s}.power-switch-track:after{content:"";position:absolute;top:3px;left:3px;width:14px;height:14px;border-radius:50%;background:#fff;transition:transform .18s}.power-source-toggle[aria-pressed="true"] .power-switch-track{background:#23aa7c}.power-source-toggle[aria-pressed="true"] .power-switch-track:after{transform:translateX(14px)}.power-source-toggle:disabled{cursor:not-allowed;opacity:.55}
.alert-sound-button{position:relative;width:42px;height:42px;display:grid;place-items:center;border:1px solid #39516c;border-radius:12px;background:#1a3152;color:#d2e3fb}.alert-sound-button:hover{background:#254368}.alert-sound-button svg{width:20px;height:20px}.alert-sound-button .sound-waves{display:none}.alert-sound-button.sound-on .sound-waves{display:block}.alert-sound-button.sound-on .sound-muted{display:none}
@media(max-width:680px){
 .sidebar{align-items:stretch}
 .sidebar-head{min-height:44px}
 .sidebar-head .sidebar-label{display:none}
 .sidebar-menu-toggle{width:100%;height:44px;display:flex;justify-content:flex-start;gap:10px;padding:0 12px}
 .sidebar-toggle-text{position:static;width:auto;height:auto;margin:0;clip:auto;overflow:visible;white-space:nowrap;font-size:13px;font-weight:700}
 .sidebar-collapsed .sidebar-head{justify-content:flex-start}
 .sidebar-collapsed .side-nav{display:none}
 .sidebar-collapsed .sidebar-menu-toggle{justify-content:flex-start}
 .header-tools{flex-wrap:wrap}
 .alert-sound-button{width:38px;height:38px;flex:none}
 .power-source-toggle{min-height:38px;gap:6px;padding:6px 8px;font-size:11px}
}
.header-tools{flex:0 0 auto;min-width:0;margin-left:auto;justify-content:flex-end;gap:5px}
.connection-clock{min-width:108px;align-items:flex-end;gap:4px;flex:none}
.connection-clock .clock{min-width:0;text-align:right}
.power-source-toggle{min-height:36px;gap:6px;padding:6px 9px;border-radius:10px;font-size:11px}
.power-switch-track{width:30px;height:18px}
.power-switch-track:after{width:12px;height:12px}
.power-source-toggle[aria-pressed="true"] .power-switch-track:after{transform:translateX(12px)}
.alert-sound-button,.notification-button{width:36px;height:36px;border-radius:10px}
.alert-sound-button svg,.notification-button svg{width:18px;height:18px}
@media(max-width:680px){
 .header-tools{display:flex;width:100%;justify-content:flex-end;gap:8px;flex-wrap:nowrap}
 .connection-clock{min-width:0;justify-self:end}
 .connection-clock .clock{text-align:right}
 .alert-sound-button,.notification-button{width:36px;height:36px}
}
@media(max-width:400px){
 .header-tools{gap:5px}
 .power-source-toggle{min-height:34px;gap:4px;padding:5px 6px;font-size:10px}
 #powerToggleState{display:none}
 .power-switch-track{width:26px;height:16px}
 .power-switch-track:after{width:10px;height:10px}
 .power-source-toggle[aria-pressed="true"] .power-switch-track:after{transform:translateX(10px)}
 .alert-sound-button,.notification-button{width:32px;height:32px}
 .alert-sound-button svg,.notification-button svg{width:16px;height:16px}
 .connection-clock{min-width:0}
 .clock strong{font-size:12px}
 .clock small{font-size:9px}
}
</style>
</head>
<body>
<div class="preview" id="previewBanner" style="display:none">Sample screen only. These are not live readings.</div>
<dialog class="power-dialog" id="powerDialog" aria-labelledby="powerDialogTitle" aria-describedby="powerDialogDescription"><h2 id="powerDialogTitle">Water warning</h2><p id="powerDialogDescription"></p><div class="power-actions"><button class="power-action action-off" id="powerOffButton" type="button" aria-label="Turn Power Source off">OFF</button><button class="power-action action-on" id="powerOnButton" type="button" aria-label="Turn Power Source on">ON</button></div></dialog>
<dialog class="history-dialog" id="notificationHistory" aria-labelledby="historyTitle"><div class="history-head"><div><h2 id="historyTitle">Alerts</h2><p id="historySummary">No alerts yet</p></div><button class="history-close" id="historyClose" type="button" aria-label="Close alerts">&times;</button></div><div class="history-list" id="notificationHistoryList" aria-live="polite"><div class="history-empty">No alerts yet. New alerts will show here.</div></div></dialog>
<header><div class="nav"><div class="brand"><div class="logo">≋</div><div><strong>S.E.C.U.R.E</strong><small>Water level</small></div></div><div class="header-tools"><button class="power-source-toggle" id="powerSourceToggle" type="button" aria-label="Power Source is off" aria-pressed="false" disabled><span>Power Source</span><span class="power-switch-track" aria-hidden="true"></span><span id="powerToggleState">Off</span></button><span class="sr-only" id="powerToggleMessage" role="status" aria-live="polite"></span><button class="alert-sound-button" id="alertSoundButton" type="button" aria-label="Turn on alert sound" aria-pressed="false" title="Turn on alert sound"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M11 5 6 9H3v6h3l5 4V5Z"/><path class="sound-waves" d="M15.5 8.5a5 5 0 0 1 0 7M18.5 5.5a9 9 0 0 1 0 13"/><path class="sound-muted" d="m16 9 5 6m0-6-5 6"/></svg></button><span class="sr-only" id="alertSoundStatus" role="status" aria-live="polite">Sound is off. Tap the sound button to enable alert sounds.</span><button class="notification-button" id="notificationButton" type="button" aria-label="Alerts: 0" title="Show alerts"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M18 8a6 6 0 0 0-12 0c0 7-3 7-3 9h18c0-2-3-2-3-9"/><path d="M10 21h4"/></svg><span class="notification-count" id="notificationCount" hidden>0</span></button><div class="connection-clock" role="status" aria-label="Connection status and local date and time"><span class="dot" id="linkDot" aria-label="Connecting"></span><div class="clock" aria-label="Local date and time"><strong id="clockTime">--:-- --</strong><small id="clockDate">Loading date...</small></div></div></div></div></header>
<div class="notification-slot"><aside class="toast" id="statusToast" aria-live="polite" aria-atomic="true" hidden><button class="toast-main" id="toastDetails" type="button" disabled><span class="toast-icon" id="toastIcon">!</span><span class="toast-copy"><strong id="toastTitle"></strong><span id="toastMessage"></span></span></button><button class="toast-close" id="toastClose" type="button" aria-label="Close notification">&times;</button></aside></div>
<div class="app-shell" id="appShell">
<aside class="sidebar"><div class="sidebar-head"><div class="sidebar-label">MENU</div><button class="sidebar-menu-toggle" id="menuToggle" type="button" aria-label="Collapse menu" aria-expanded="true" aria-controls="sideNav" title="Collapse menu"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M4 6h16M4 12h16M4 18h16"/></svg><span class="sidebar-toggle-text">Hide menu</span></button></div><nav class="side-nav" id="sideNav" aria-label="Main menu"><button class="side-link" type="button" data-view="dashboardView" aria-controls="dashboardView" aria-pressed="true"><span class="side-icon" aria-hidden="true">⌂</span><span>Home</span></button><button class="side-link" type="button" data-view="monitoringView" aria-controls="monitoringView" aria-pressed="false"><span class="side-icon" aria-hidden="true">◉</span><span>Water history</span></button><button class="side-link" type="button" data-view="settingsView" aria-controls="settingsView" aria-pressed="false"><span class="side-icon" aria-hidden="true">⚙</span><span>Settings</span></button></nav><div class="sidebar-note">Water readings update while you move between pages.</div></aside>
<main>
<section class="app-view" id="dashboardView">
<div class="pagehead"><div><div class="eyebrow">Water level</div><h1>Water right now</h1><p class="sub">See how high the water is. A smaller distance means the water is closer.</p></div><div class="updated" id="updated">Waiting for readings...</div></div>
<div class="cols">
 <section class="card status-card"><div><div class="card-title">WATER LEVEL</div><div class="status-title offline" id="mainStatus">Getting readings...</div><p class="status-desc" id="mainDescription">Getting the latest water reading.</p></div><div class="condition" id="condition">Checking water reader...</div></section>
 <section class="card level-card"><div><p class="card-title">WATER IS THIS FAR BELOW THE DANGER LEVEL</p><div class="metric"><strong id="level">—</strong><span>centimeters</span></div><div class="helper" id="levelNote">The smaller the number, the closer the water is to the device.</div><div class="mini"><div><span>Water rise estimate</span><b id="height">—</b></div><div><span>Tank level</span><b id="distance">—</b></div></div></div><div class="tank" role="img" aria-label="Picture showing how high the water is"><div class="tank-water" id="tankWater"></div><div class="tank-text" id="tankText">—</div></div></section>
</div>
<div class="stats-title"><h2>More details</h2><span>Updates every second</span></div>
<div class="sensors"><div class="sensor"><small>Water reader</small><strong id="ultraHealth">Checking</strong><div class="secondary">Checks water level</div></div><div class="sensor"><small>Estimated water rise</small><strong id="heightCard">—</strong><div class="secondary">Compared with an empty tank</div></div><div class="sensor"><small>Water distance</small><strong id="distanceCard">—</strong><div class="secondary">Below the device</div></div><div class="sensor"><small>Water level</small><strong id="alertLevel">—</strong><div class="secondary">Based on distance</div></div></div>
 </section>
<section class="app-view" id="monitoringView" hidden>
<div class="pagehead"><div><div class="eyebrow">Past readings</div><h1>Water history</h1><p class="sub">See how the water level has changed and when the alarm sounded.</p></div></div>
<div class="lower"><section class="card"><div class="charthead"><b>Water level over time</b><span>Last 60 readings</span></div><canvas id="trend" aria-label="Water level history chart"></canvas><div class="hint" style="margin-top:14px">The chart shows how far the water is below the device, in centimeters. It keeps up to 60 readings and clears when you close this page.</div></section><section class="card"><div class="charthead"><b>Lights and sound</b><span>Right now</span></div><div class="ledrow"><div class="lightbox"><span class="led green" id="ledGreen"></span><b>Water is safe</b></div><div class="lightbox"><span class="led yellow" id="ledYellow"></span><b>Water is rising</b></div><div class="lightbox"><span class="led red" id="ledRed"></span><b>Water is too high</b></div></div><div class="hrow"><span>Power Source indicator</span><strong id="monitorLoadLed">—</strong></div><div class="hrow"><span>Alarm sound</span><strong id="buzzer">—</strong></div><div class="hrow"><span>Alarm sound style</span><strong id="buzzerMode">—</strong></div><div class="hrow"><span>Water change to record</span><strong id="changeAlerts">—</strong></div><div class="hrow"><span>Last water change</span><strong id="changeDelta">—</strong></div><div class="hrow"><span>Changes recorded</span><strong id="changeCount">0</strong></div><div class="hrow"><span>Phones connected</span><strong id="clients">—</strong></div><div class="hrow"><span>Time since start</span><strong id="uptime">—</strong></div><div class="test-panel"><div class="field" style="margin-bottom:12px"><label for="driverMode">Choose your alarm</label><select id="driverMode" class="driver-select"><option value="active">Alarm that makes its own sound</option><option value="passive">Alarm that needs the device to make sound</option></select></div><button class="btn" type="button" id="applyBuzzerModeBtn" style="margin-right:7px">Save choice</button><button class="btn" type="button" id="testBuzzerBtn">Test alarm sound</button><div class="hint">Choose the option that matches your alarm. The alarm sounds when the water is rising or too high. If you hear nothing during the test, check the connections.</div><div id="buzzerTestMessage" class="message" aria-live="polite"></div></div></section></div>
 </section>
<section class="app-view" id="settingsView" hidden>
<div class="pagehead"><div><div class="eyebrow">Water settings</div><h1>Settings</h1><p class="sub">See when the water is safe, rising, or too high.</p></div></div>
<div class="cal-head"><div><h2>When do alerts happen?</h2><p>The device measures how far the water is below it. A smaller number means the water is higher.</p></div></div>
<section class="card"><div class="hrow"><span>Water is safe</span><strong>11 centimeters or more below the device</strong></div><div class="hrow"><span>Water is rising — slow beeps</span><strong>7 to under 11 centimeters below the device</strong></div><div class="hrow"><span>Water is too high — fast beeps</span><strong>Less than 7 centimeters below the device</strong></div><div class="hrow"><span>No reading</span><strong>The reader cannot see the water</strong></div><div class="hrow"><span>Power Source indicator when water is safe</span><strong>Turns on by itself, unless turned off after a high-water alert</strong></div><div class="hrow"><span>Power Source indicator when water is rising</span><strong>Keeps its current setting</strong></div><div class="hrow"><span>Power Source indicator when water is too high</span><strong>Turns off by itself</strong></div><div class="hrow"><span>Power Source indicator when there is no reading</span><strong>Keeps its current setting</strong></div><p class="hint" style="margin:15px 0">The reader may not work when the water is very close. Keep the device dry and check it often. Do not use it as your only flood warning.</p><form id="configForm"><div class="config-grid"><div class="field"><label for="changeCm">Smallest water change to record (centimeters)</label><input id="changeCm" name="changeCm" type="number" min="0.5" max="20" step="0.5" required placeholder="1"></div></div><div class="settings-bottom"><div class="hint">Choose how much the water must move before the change is saved. This does not change when the alarm sounds.</div><button class="btn" id="saveBtn" type="submit">Save setting</button></div><div class="message" id="saveMessage" aria-live="polite"></div></form></section>
 </section>
</main></div><footer class="footer">The Power Source switch controls the device indicator only. It cannot turn off power to your home. Keep the device dry. Do not use it as your only flood warning.</footer>
<script>
const el=id=>document.getElementById(id);
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
let samples=[],formLoaded=false,polling=false,previewTick=0,previewLedOn=false,lastStatus=null,lastLedState=null,toastTimer=0,toastHideTimer=0,notificationsReceived=0,lastNotification=null,notificationHistory=[];
if(preview){el('previewBanner').style.display='block';el('powerSourceToggle').disabled=true;el('powerOffButton').disabled=true;el('powerOnButton').disabled=true;el('saveBtn').disabled=true;el('testBuzzerBtn').disabled=true;el('applyBuzzerModeBtn').disabled=true;el('saveMessage').textContent='Connect to the device to save this setting.';el('buzzerTestMessage').textContent='Connect to the device to test the alarm.';}
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
function offline(){el('linkDot').classList.remove('on');el('linkDot').classList.add('offline');el('linkDot').setAttribute('aria-label','Not connected');el('updated').textContent='Connection lost';el('mainStatus').textContent='No readings';el('mainStatus').className='status-title offline';el('mainDescription').textContent='Connect your phone to the S.E.C.U.R.E network and open 192.168.4.1.';el('condition').textContent='Water level is not available';el('ultraHealth').textContent='No reading';el('heightCard').textContent='—';el('level').textContent='—';el('tankText').textContent='—';el('tankWater').style.height='0%';el('height').textContent='—';el('distance').textContent='—';el('distanceCard').textContent='—';el('alertLevel').textContent='—';el('buzzer').textContent='—';el('buzzerMode').textContent='—';el('changeAlerts').textContent='—';el('changeDelta').textContent='—';el('changeCount').textContent='—';el('clients').textContent='—';el('uptime').textContent='—';setLED('ledGreen',false);setLED('ledYellow',false);setLED('ledRed',false);el('monitorLoadLed').textContent='—';el('powerSourceToggle').disabled=true;}
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
el('level').textContent=d.ultrasonicOk?Number(d.distanceCm).toFixed(1):'—';el('tankText').textContent=d.ultrasonicOk?Math.round(d.levelPercent)+'%':'—';el('tankWater').style.height=(d.ultrasonicOk?Math.max(0,Math.min(100,d.levelPercent)):0)+'%';el('levelNote').textContent='Smaller numbers mean the water is closer. The tank picture is an estimate.';
el('height').textContent=d.ultrasonicOk?Number(d.waterHeightCm).toFixed(1)+' centimeters':'No reading';el('distance').textContent=d.ultrasonicOk?Math.round(d.levelPercent)+'%':'No reading';el('distanceCard').textContent=d.ultrasonicOk?Number(d.distanceCm).toFixed(1)+' centimeters':'No reading';el('ultraHealth').textContent=d.ultrasonicOk?'Working':'No reading';el('heightCard').textContent=d.ultrasonicOk?Number(d.waterHeightCm).toFixed(1)+' centimeters':'No reading';el('alertLevel').textContent=simpleStatus(d.status);
setLED('ledGreen',d.green);setLED('ledYellow',d.yellow);setLED('ledRed',d.red);el('buzzer').textContent=d.buzzer?'On':'Off';el('buzzer').className=d.buzzer?'alarm':'';el('buzzerMode').textContent=d.buzzerType==='passive'?'Needs the device to make sound':'Makes its own sound';el('changeAlerts').textContent=Number(d.changeAlertCm).toFixed(1)+' centimeters';el('changeDelta').textContent=d.lastAlertDeltaCm>=0?Number(d.lastAlertDeltaCm).toFixed(1)+' centimeters · '+(d.lastAlertDirection==='Rising water'?'Water going up':'Water going down'):'Waiting for a change';el('changeCount').textContent=d.distanceChangeAlertCount||0;el('clients').textContent=d.clients;el('uptime').textContent=secs(d.uptime);
if(!formLoaded){el('changeCm').value=d.changeAlertCm;el('driverMode').value=d.buzzerType;formLoaded=true}
samples.push(d.ultrasonicOk?d.distanceCm:null);if(samples.length>60)samples.shift();draw();}
function draw(){const c=el('trend');const r=c.getBoundingClientRect(),w=Math.max(180,r.width),h=Math.max(100,r.height),scale=window.devicePixelRatio||1;c.width=Math.round(w*scale);c.height=Math.round(h*scale);const x=c.getContext('2d');x.scale(scale,scale);const left=27,right=w-9,top=9,bottom=h-19,max=Math.max(16,Math.ceil(Math.max(0,...samples.filter(v=>v!==null&&Number.isFinite(v)))/4)*4);x.strokeStyle='#e7edf5';x.lineWidth=1;x.font='10px Arial';x.fillStyle='#96a4b7';for(let t=0;t<=4;t++){const y=top+(bottom-top)*t/4;x.beginPath();x.moveTo(left,y);x.lineTo(right,y);x.stroke();x.fillText(String(max-t*max/4),2,y+3)}let active=false;const values=samples;values.forEach((v,i)=>{if(v===null||v>max){active=false;return}const xx=left+(right-left)*i/Math.max(59,values.length-1),yy=bottom-(bottom-top)*v/max;if(!active){x.beginPath();x.moveTo(xx,yy);active=true}else x.lineTo(xx,yy);x.strokeStyle='#2b70e9';x.lineWidth=2.6;x.lineJoin='round';x.stroke()});const idx=values.length-1;if(idx>=0&&values[idx]!==null&&values[idx]<=max){const xx=left+(right-left)*idx/Math.max(59,values.length-1),yy=bottom-(bottom-top)*values[idx]/max;x.fillStyle='#2b70e9';x.beginPath();x.arc(xx,yy,3.8,0,2*Math.PI);x.fill()}}
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

  if (status != previous) {
    statusSinceMs = millis();
  }
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
  const uint32_t statusElapsedMs = now - statusSinceMs;
  const bool warningBeep = status == "warning" &&
                           statusElapsedMs % (WARNING_BUZZER_ON_MS + WARNING_BUZZER_OFF_MS) < WARNING_BUZZER_ON_MS;
  const bool criticalBeep = status == "critical" &&
                            statusElapsedMs % (CRITICAL_BUZZER_ON_MS + CRITICAL_BUZZER_OFF_MS) < CRITICAL_BUZZER_ON_MS;
  const bool powerOffDelayElapsed = !loadLedOn && loadLedOffTimerActive &&
                                    now - loadLedOffSinceMs >= BUZZER_POWER_OFF_DELAY_MS;
  const bool sound = !powerOffDelayElapsed && (manualTest || warningBeep || criticalBeep);
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

void applyCloudControlCommand(const String &responseBody) {
  const int commandStart = responseBody.indexOf("\"command\":{");
  if (commandStart < 0) return;

  const String commandId = cloudJsonStringField(responseBody, "id", commandStart);
  const String action = cloudJsonStringField(responseBody, "action", commandStart);
  if (commandId.length() != 36 || (action != "on" && action != "off")) return;
  if (prefs.getString("commandAck", "") == commandId) return;

  const bool turnOn = action == "on";
  loadLedAutoOffLatched = !turnOn;
  setLoadLed(turnOn);
  prefs.putString("commandAck", commandId);
  Serial.printf("Cloud control command %s acknowledged: GPIO23 %s\n",
                commandId.c_str(), turnOn ? "ON" : "OFF");
}

void sendCloudTelemetry() {
  if (strlen(AQUAGUARD_WIFI_SSID) == 0 || strlen(AQUAGUARD_DEVICE_TOKEN) < 32) return;

  const uint32_t now = millis();
  if (now - lastCloudTelemetryMs < CLOUD_TELEMETRY_INTERVAL_MS) return;
  lastCloudTelemetryMs = now;
  const wl_status_t wifiStatus = WiFi.status();
  if (wifiStatus != WL_CONNECTED) {
    Serial.printf("Cloud telemetry skipped: router Wi-Fi status %d\n", static_cast<int>(wifiStatus));
    return;
  }
  Serial.printf("Cloud telemetry Wi-Fi connected: IP %s, signal %d dBm\n",
                WiFi.localIP().toString().c_str(), WiFi.RSSI());

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

  WiFiClientSecure tlsClient;
  tlsClient.setCACert(VERCEL_ROOT_CA);
  HTTPClient http;
  if (!http.begin(tlsClient, AQUAGUARD_TELEMETRY_URL)) {
    Serial.println("ERROR: Could not start HTTPS telemetry request");
    return;
  }

  http.setConnectTimeout(2500);
  http.setTimeout(2500);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", String("Bearer ") + AQUAGUARD_DEVICE_TOKEN);
  const int responseCode = http.POST(buildStatusJson());
  if (responseCode < 200 || responseCode >= 300) {
    Serial.printf("ERROR: Cloud telemetry request returned HTTP %d\n", responseCode);
  } else {
    const String responseBody = http.getString();
    applyCloudControlCommand(responseBody);
  }
  http.end();
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
  // Short startup self-test (every reset): confirms GPIO21 alarm control.
  Serial.println("Startup buzzer self-test: 600 ms");
  setBuzzer(true);
  delay(600);
  setBuzzer(false);

  pinMode(ULTRA_TRIG_PIN, OUTPUT);
  pinMode(ULTRA_ECHO_PIN, INPUT);
  digitalWrite(ULTRA_TRIG_PIN, LOW);

  prefs.begin("aquaguard", false);
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
  } else {
    Serial.println("Cloud telemetry disabled: configure router Wi-Fi in secrets.h");
  }
  dns.start(53, "*", WiFi.softAPIP());
  server.on("/", HTTP_GET, sendPage);
  server.on("/api/status", HTTP_GET, sendStatus);
  server.on("/api/led", HTTP_POST, postLoadLed);
  server.on("/api/config", HTTP_POST, postConfig);
  server.on("/api/buzzer/test", HTTP_POST, postTestBuzzer);
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
