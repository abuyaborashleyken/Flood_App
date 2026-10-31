# S.E.C.U.R.E-App water level monitor

The ESP32 keeps its local dashboard at `http://192.168.4.1` and can also send
sensor readings to the live S.E.C.U.R.E-App dashboard:

https://secure-app-live.vercel.app

## Connect the ESP32 to the internet

1. The router Wi-Fi credentials and generated device token are configured in
   the local `SECURE_MONITORING/secrets.h`. Do not replace that file with the
   example: it is Git-ignored and must remain private.
2. Open `SECURE_MONITORING/SECURE_MONITORING.ino` in Arduino IDE, install the
   ESP32 board support if needed, select the connected ESP32 board and port,
   then upload the sketch.

The ESP32 continues to broadcast its local `S.E.C.U.R.E` network while joining
the router network. The Vercel dashboard uses the same design as the local
device dashboard. Cloud readings are sent every second and the dashboard
refreshes every second. The dashboard can remotely switch the low-voltage
GPIO23 indicator when the ESP32 is online. Remote commands are password
protected and expire if the device does not receive them within 30 seconds.
The control password must be configured as the Vercel environment variable
`AQUAGUARD_CONTROL_PASSWORD` and must be at least 16 characters. The browser
session expires after eight hours; use the **Lock** button to end it sooner.
On the live dashboard, click the **Power Source** indicator and enter this
password to unlock remote control. The cloud dashboard cannot switch a pump,
mains circuit, or other power supply. Wi-Fi and cloud latency mean readings
are near-real-time, not instantaneous.

## S.E.C.U.R.E-App deployment

The Vercel project is `secure-app`, published at
https://secure-app-live.vercel.app. The previous
`aquaguard-flood-dashboard.vercel.app` address remains as a compatibility
alias for already-flashed ESP32 firmware; the checked-in sketch now targets
the new address. Upload the sketch to the ESP32 to switch its telemetry
endpoint. The dashboard uses the supplied S.E.C.U.R.E logo as its app mark,
favicon, and install icon. Its rounded layout adapts to mobile, tablet, and
desktop displays. Its private Blob store
contains the latest sensor reading and a short-lived pending indicator
command. The device token, control password, and Blob credentials are managed
as Vercel environment variables, not in this repository.

## Install S.E.C.U.R.E-App

Open the dashboard over HTTPS in a supported browser and choose **Install app**
in the header. On iPhone or iPad, open the dashboard in Safari, tap **Share**,
then choose **Add to Home Screen**. On Android, use the browser menu and select
**Install app** or **Add to Home screen**. The app shell and branding assets are
available offline after the first visit; live readings and remote controls
still require an internet connection.

For code changes, run `npm install` and `npm test`, then push to `main`.
The Vercel project is connected to this GitHub repository and deploys changes
from authorized commits automatically.
