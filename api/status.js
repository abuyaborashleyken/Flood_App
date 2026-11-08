import { readTelemetry } from "../lib/supabase-store.js";
import { isTelemetryFresh } from "../lib/telemetry.js";
import { applyCors } from "../lib/cors.js";

export default async function handler(req, res) {
  if (!applyCors(req, res)) return;
  res.setHeader("Cache-Control", "no-store");

  if (req.method !== "GET") {
    res.setHeader("Allow", "GET");
    return res.status(405).json({ ok: false, error: "Method not allowed." });
  }

  try {
    const telemetry = await readTelemetry();
    if (!telemetry) {
      return res.status(200).json({ ok: true, available: false });
    }

    if (!isTelemetryFresh(telemetry.receivedAt)) {
      return res.status(200).json({ ok: true, available: false });
    }

    return res.status(200).json({ ok: true, available: true, ...telemetry });
  } catch (error) {
    console.error("Could not read S.E.C.U.R.E-App telemetry.", error);
    return res.status(503).json({ ok: false, error: "Could not load telemetry." });
  }
}
