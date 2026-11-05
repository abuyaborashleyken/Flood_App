import {
  broadcastTelemetry,
  clearPendingCommand,
  readPendingCommand,
  saveTelemetry,
} from "../lib/supabase-store.js";
import {
  isAuthorizedAuthorizationHeader,
  normalizeTelemetry,
} from "../lib/telemetry.js";

export default async function handler(req, res) {
  res.setHeader("Cache-Control", "no-store");

  if (req.method !== "POST") {
    res.setHeader("Allow", "POST");
    return res.status(405).json({ ok: false, error: "Method not allowed." });
  }

  const deviceToken = process.env.AQUAGUARD_DEVICE_TOKEN;
  if (!deviceToken) {
    console.error("AQUAGUARD_DEVICE_TOKEN is not configured.");
    return res.status(500).json({ ok: false, error: "Telemetry service is not configured." });
  }

  if (!isAuthorizedAuthorizationHeader(req.headers.authorization, deviceToken)) {
    return res.status(401).json({ ok: false, error: "Unauthorized device." });
  }

  const telemetry = normalizeTelemetry(req.body, new Date().toISOString());
  if (!telemetry) {
    return res.status(400).json({ ok: false, error: "Invalid telemetry payload." });
  }

  try {
    await saveTelemetry(telemetry);
    const broadcastPromise = process.env.SUPABASE_ANON_KEY
      ? broadcastTelemetry(telemetry).catch(error => {
        console.error("Could not broadcast the latest telemetry to connected dashboards.", error);
      })
      : Promise.resolve();
    const [command] = await Promise.all([readPendingCommand(), broadcastPromise]);
    if (!command) return res.status(200).json({ ok: true });

    if (
      typeof command.id !== "string" ||
      typeof command.action !== "string" ||
      !["on", "off"].includes(command.action) ||
      !["led", "buzzer"].includes(command.target) ||
      !Number.isFinite(command.expiresAt)
    ) {
      console.error("Discarding malformed AquaGuard remote control command.");
      if (typeof command.id === "string") await clearPendingCommand(command.id);
      return res.status(200).json({ ok: true });
    }

    if (telemetry.commandAck === command.id || command.expiresAt <= Date.now()) {
      await clearPendingCommand(command.id);
      return res.status(200).json({ ok: true });
    }

    return res.status(200).json({
      ok: true,
      command: { id: command.id, action: command.action, target: command.target },
    });
  } catch (error) {
    console.error("Could not save telemetry or read a pending command.", error);
    return res.status(503).json({
      ok: false,
      error: "Could not save telemetry or read a pending command.",
    });
  }
}
