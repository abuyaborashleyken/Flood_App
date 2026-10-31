# AquaGuard flood monitor

The ESP32 keeps its local dashboard at `http://192.168.4.1` and can also send
sensor readings to the live Vercel dashboard:

https://aquaguard-flood-dashboard.vercel.app

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
refreshes every second. The cloud dashboard is read-only; controls on the
local dashboard continue to work only on the ESP32 network. Wi-Fi and cloud
latency mean readings are near-real-time, not instantaneous.

## Vercel project

The Vercel project is `aquaguard-flood-dashboard`. Its private Blob store
contains only the latest sensor reading. The device token and Blob credentials
are managed as Vercel environment variables, not in this repository.

For code changes, run `npm install` and `npm test`, then push to `main`.
The Vercel project is connected to this GitHub repository and deploys changes
from authorized commits automatically.
