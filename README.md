# S.E.C.U.R.E APPLICATION water level monitor

The ESP32 keeps its local dashboard at `http://192.168.4.1` and can also send
sensor readings to the live S.E.C.U.R.E APPLICATION dashboard:

https://secure-app-live.vercel.app

## Connect the ESP32 to the internet

1. Configure the router Wi-Fi credentials and a device token of at least 32
   characters in the local `SECURE_MONITORING/secrets.h`. Set the same token
   as the Vercel `AQUAGUARD_DEVICE_TOKEN` environment variable. Do not replace
   `secrets.h` with the example: it is Git-ignored and must remain private.
2. Open `SECURE_MONITORING/SECURE_MONITORING.ino` in Arduino IDE, install the
   ESP32 board support if needed, select the connected ESP32 board and port,
   then upload the sketch.

The ESP32 continues to broadcast its local `S.E.C.U.R.E` network while joining
the router network. The Vercel dashboard uses the same design as the local
device dashboard. Cloud readings are sent about once a second and are pushed
to connected dashboards as soon as they arrive. The dashboard falls back to
one-second polling if the Realtime connection is unavailable. The ESP32 sends
readings from a background network
task, so slow cloud requests do not block sensor sampling or the local
dashboard; when a request is delayed, only the newest pending reading is kept.
The dashboard can remotely switch the low-voltage GPIO23 indicator when the
ESP32 is online. The buzzer repeats a 0.5-second beep with a 0.5-second pause
in WARNING, and a 0.2-second beep with a 0.2-second pause in CRITICAL; it stops
when the status returns to SAFE. Remote commands are password protected and
expire if the device does not receive them within 30 seconds. The Water
history page has a buzzer toggle; turning it off silences hardware and turning
it on restores WARNING/CRITICAL alarms. The cloud dashboard requires the
remote-control password to change this setting.
The control password must be configured as the Vercel environment variable
`AQUAGUARD_CONTROL_PASSWORD` and must be at least 16 characters. The browser
session expires after eight hours; use the **Lock** button to end it sooner.
On the live dashboard, click **Power Source** or save the water-change
sensitivity setting and enter this password to unlock remote control. Settings
are saved to the ESP32 only after it confirms the command. The cloud dashboard
cannot switch a pump, mains circuit, or other power supply. Wi-Fi and cloud
latency mean readings are near-real-time, not instantaneous.

## S.E.C.U.R.E APPLICATION deployment

The Vercel project is `secure-app`, published at
https://secure-app-live.vercel.app. The previous
`aquaguard-flood-dashboard.vercel.app` address remains as a compatibility
alias for already-flashed ESP32 firmware; the checked-in sketch now targets
the new address. Upload the sketch to the ESP32 to switch its telemetry
endpoint. The dashboard uses the supplied S.E.C.U.R.E logo as its app mark,
favicon, and install icon. Its rounded layout adapts to mobile, tablet, and
desktop displays. A private Supabase database stores the latest sensor reading
and short-lived indicator commands. The device token, control password, and
Supabase service-role key are managed as Vercel environment variables, not in
this repository. A Supabase publishable/anon key is exposed only for the
read-only Realtime WebSocket client; it is not a service-role key.

### Configure the Supabase telemetry backend

1. Create a Supabase project and open its **SQL Editor**.
2. Run `supabase/migrations/20261031000000_create_monitoring_tables.sql`.
   Also run `supabase/migrations/20261104000000_add_remote_buzzer_control.sql`
   to enable authenticated remote buzzer control, and
   `supabase/migrations/20261105000000_allow_secure_telemetry_realtime.sql`
   to authorize read-only dashboard subscriptions to the private telemetry
   broadcast channel, and
   `supabase/migrations/20261106000000_allow_remote_change_alert_setting.sql`
   to allow authenticated remote saving of the water-change sensitivity. In
   Supabase **Realtime settings**, disable **Allow public
   access** so broadcast access is controlled by this policy.
   The tables have row-level security enabled; only the Vercel server uses the
   service-role key, which must never be exposed in browser code.
3. In the Vercel project, add `SUPABASE_URL` and the sensitive
   `SUPABASE_SERVICE_ROLE_KEY` for Production. Add them to Preview and
   Development too if those environments should use the database. Add the
   Supabase publishable/anon key as `SUPABASE_ANON_KEY`; this public key is
   used only by the browser to subscribe to telemetry.
4. Redeploy `secure-app` so its serverless API picks up the new variables.

The ESP32 continues to post to `/api/telemetry`; Vercel validates each device
token, saves each reading, and broadcasts it immediately to connected
dashboards. The dashboard falls back to one-second status polling if Realtime
is unavailable and checks device connectivity every ten seconds while the
push connection is healthy. No hardware protocol change or additional firmware
secret is needed for this backend swap. Upload the current ESP32 sketch to enable
the cloud dashboard's authenticated water-change sensitivity setting; the
device persists the setting and confirms it through its next telemetry update.
### Enable realtime delivery

The dashboard subscribes to Supabase Realtime broadcasts and keeps the one-second
status polling as a fallback. Apply
`supabase/migrations/20261105000000_allow_secure_telemetry_realtime.sql` and set
`SUPABASE_ANON_KEY` in Vercel to the Supabase publishable key. This key is safe
for browser use; keep `SUPABASE_SERVICE_ROLE_KEY` server-only.

## Install S.E.C.U.R.E APPLICATION

Open the dashboard over HTTPS in a supported browser and choose **Install app**
in the header. On iPhone or iPad, open the dashboard in Safari, tap **Share**,
then choose **Add to Home Screen**. On Android, use the browser menu and select
**Install app** or **Add to Home screen**. The app shell and branding assets are
available offline after the first visit; live readings and remote controls
still require an internet connection.

For code changes, run `npm install` and `npm test`, then push to `main`.
The Vercel project is connected to this GitHub repository and deploys changes
from authorized commits automatically.
