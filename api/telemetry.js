import { put } from "@vercel/blob";
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
    await put("telemetry/latest.json", JSON.stringify(telemetry), {
      access: "private",
      addRandomSuffix: false,
      allowOverwrite: true,
      contentType: "application/json",
      cacheControlMaxAge: 0,
    });
    return res.status(200).json({ ok: true });
  } catch (error) {
    console.error("Could not save AquaGuard telemetry.", error);
    return res.status(503).json({ ok: false, error: "Could not save telemetry." });
  }
}
