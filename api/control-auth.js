import {
  CONTROL_COOKIE_NAME,
  CONTROL_SESSION_MAX_AGE_SECONDS,
  createControlSession,
  isAuthorizedControlCookie,
  isSameOriginRequest,
  isValidControlPassword,
  MIN_CONTROL_PASSWORD_LENGTH,
} from "../lib/control-auth.js";

export default function handler(req, res) {
  res.setHeader("Cache-Control", "no-store");
  res.setHeader("Vary", "Cookie");

  const controlPassword = process.env.AQUAGUARD_CONTROL_PASSWORD;
  if (req.method === "GET") {
    return res.status(200).json({
      ok: true,
      configured: isConfigured(controlPassword),
      authenticated: isAuthorizedControlCookie(
        req.headers.cookie,
        controlPassword,
      ),
    });
  }

  if (req.method === "DELETE") {
    res.setHeader(
      "Set-Cookie",
      `${CONTROL_COOKIE_NAME}=; HttpOnly; Secure; SameSite=Strict; Path=/api; Max-Age=0`,
    );
    return res.status(200).json({ ok: true });
  }

  if (req.method !== "POST") {
    res.setHeader("Allow", "GET, POST, DELETE");
    return res.status(405).json({ ok: false, error: "Method not allowed." });
  }

  if (!isSameOriginRequest(req)) {
    return res.status(403).json({ ok: false, error: "Request origin rejected." });
  }

  if (!isConfigured(controlPassword)) {
    console.error("AQUAGUARD_CONTROL_PASSWORD is not configured.");
    return res.status(503).json({
      ok: false,
      error: "Remote control is not configured.",
    });
  }

  if (!isValidControlPassword(req.body?.password, controlPassword)) {
    return res.status(401).json({ ok: false, error: "Incorrect control password." });
  }

  const session = createControlSession(controlPassword);
  res.setHeader(
    "Set-Cookie",
    `${CONTROL_COOKIE_NAME}=${session.value}; HttpOnly; Secure; SameSite=Strict; Path=/api; Max-Age=${CONTROL_SESSION_MAX_AGE_SECONDS}`,
  );
  return res.status(200).json({ ok: true, authenticated: true });
}

function isConfigured(password) {
  return (
    typeof password === "string" &&
    Buffer.byteLength(password) >= MIN_CONTROL_PASSWORD_LENGTH
  );
}
