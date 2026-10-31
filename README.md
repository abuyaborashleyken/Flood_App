# AquaGuard flood monitor

The ESP32 keeps its local dashboard at `http://192.168.4.1` and can also send
sensor readings to the live Vercel dashboard:

https://aquaguard-flood-dashboard.vercel.app

## Connect the ESP32 to the internet

1. Copy `SECURE_MONITORING/secrets.example.h` to
   `SECURE_MONITORING/secrets.h`.
2. Set `AQUAGUARD_WIFI_SSID` and `AQUAGUARD_WIFI_PASSWORD` to the 2.4 GHz
   router network the ESP32 can join. Keep `secrets.h` private; Git ignores it.
3. Keep the generated `AQUAGUARD_DEVICE_TOKEN` in `secrets.h`. It must match
   the Vercel project secret and must not be shared or committed.
4. Open `SECURE_MONITORING/SECURE_MONITORING.ino` in Arduino IDE, install the
   ESP32 board support if needed, select the connected ESP32 board and port,
   then upload the sketch.

The ESP32 continues to broadcast its local `S.E.C.U.R.E` network while joining
the router network. Cloud readings are sent every 30 seconds. The Vercel
dashboard refreshes automatically and shows the age of the latest reading.
The cloud dashboard is read-only; controls on the local dashboard continue to
work only on the ESP32 network.

## Vercel project

The Vercel project is `aquaguard-flood-dashboard`. Its private Blob store
contains only the latest sensor reading. The device token and Blob credentials
are managed as Vercel environment variables, not in this repository.

For code changes, run `npm install` and `npm test`, then push to `main`.
The Vercel project is connected to this GitHub repository and deploys changes
from authorized commits automatically.
