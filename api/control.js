import { randomUUID } from "node:crypto";
import { queueControlCommand } from "../lib/supabase-store.js";
import {
  isAuthorizedControlCookie,
  isSameOriginRequest,
} from "../lib/control-auth.js";

export default async function handler(req, res) {
  res.setHeader("Cache-Control", "no-store");

  if (req.method !== "POST") {
    res.setHeader("Allow", "POST");
    return res.status(405).json({ ok: false, error: "Method not allowed." });
  }

  if (!isSameOriginRequest(req)) {
    return res.status(403).json({ ok: false, error: "Request origin rejected." });
  }

  const controlPassword = process.env.AQUAGUARD_CONTROL_PASSWORD;
  if (!isAuthorizedControlCookie(req.headers.cookie, controlPassword)) {
    return res.status(401).json({ ok: false, error: "Sign in to control the device." });
  }

  const { action } = req.body ?? {};
  if (action !== "on" && action !== "off") {
    return res.status(400).json({ ok: false, error: "Action must be on or off." });
  }

  const createdAt = new Date();
  const command = {
    id: randomUUID(),
    action,
    createdAt: createdAt.toISOString(),
    expiresAt: createdAt.getTime() + 30_000,
  };

  try {
    await queueControlCommand(command);
    return res.status(202).json({ ok: true, commandId: command.id });
  } catch (error) {
    console.error("Could not queue AquaGuard remote control command.", error);
    return res.status(503).json({ ok: false, error: "Could not queue the command." });
  }
}
