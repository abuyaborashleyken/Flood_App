import { del, get, put } from "@vercel/blob";
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

    const command = await get("control/pending.json", {
      access: "private",
      useCache: false,
    });
    if (!command) return res.status(200).json({ ok: true });

    const pending = JSON.parse(await new Response(command.stream).text());
    if (
      typeof pending.id !== "string" ||
      typeof pending.action !== "string" ||
      !["on", "off"].includes(pending.action) ||
      !Number.isSafeInteger(pending.expiresAt)
    ) {
      console.error("Discarding malformed AquaGuard remote control command.");
      await del(command.blob.url, { ifMatch: command.blob.etag });
      return res.status(200).json({ ok: true });
    }

    if (telemetry.commandAck === pending.id || pending.expiresAt <= Date.now()) {
      await del(command.blob.url, { ifMatch: command.blob.etag });
      return res.status(200).json({ ok: true });
    }

    return res.status(200).json({
      ok: true,
      command: { id: pending.id, action: pending.action },
    });
  } catch (error) {
    console.error("Could not save AquaGuard telemetry or read a pending command.", error);
    return res.status(503).json({
      ok: false,
      error: "Could not save telemetry or read a pending command.",
    });
  }
}
