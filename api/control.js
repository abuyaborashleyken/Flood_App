import { randomUUID } from "node:crypto";
import { queueControlCommand } from "../lib/supabase-store.js";
import {
  isAuthorizedControlCookie,
  isSameOriginRequest,
} from "../lib/control-auth.js";
import { applyCors } from "../lib/cors.js";

export default async function handler(req, res) {
  if (!applyCors(req, res)) return;
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

  const { action, target = "led", value } = req.body ?? {};
  const controlCommand = ["on", "off"].includes(action) &&
    ["led", "buzzer"].includes(target) &&
    value === undefined;
  const settingsCommand = action === "set-change-alert" &&
    target === "settings" &&
    typeof value === "number" &&
    Number.isFinite(value) &&
    value >= 0.5 &&
    value <= 20 &&
    Number.isInteger(value * 2);
  if (!controlCommand && !settingsCommand) {
    return res.status(400).json({
      ok: false,
      error: "Choose a valid device command or a change-alert setting from 0.5 to 20 cm in 0.5 cm steps.",
    });
  }

  const createdAt = new Date();
  const command = {
    id: randomUUID(),
    action,
    target,
    value: settingsCommand ? value : null,
    createdAt: createdAt.toISOString(),
    expiresAt: createdAt.getTime() + 30_000,
  };

  try {
    await queueControlCommand(command);
    return res.status(202).json({
      ok: true,
      commandId: command.id,
      target,
      ...(settingsCommand ? { value } : {}),
    });
  } catch (error) {
    console.error("Could not queue AquaGuard remote control command.", error);
    return res.status(503).json({ ok: false, error: "Could not queue the command." });
  }
}
