export function isAllowedCorsOrigin(req) {
  const headers = req.headers || {};
  const origin = headers.origin;
  if (typeof origin !== "string") return false;

  let parsedOrigin;
  try {
    parsedOrigin = new URL(origin);
  } catch {
    return false;
  }
  if (parsedOrigin.protocol !== "https:" || parsedOrigin.origin !== origin) {
    return false;
  }

  const host = headers["x-forwarded-host"] || headers.host;
  if (typeof host === "string" && parsedOrigin.host === host) return true;

  const configuredOrigin = process.env.AQUAGUARD_APP_ORIGIN;
  if (typeof configuredOrigin !== "string") return false;
  try {
    const parsedConfiguredOrigin = new URL(configuredOrigin);
    return parsedConfiguredOrigin.protocol === "https:" &&
      parsedConfiguredOrigin.origin === configuredOrigin &&
      parsedOrigin.origin === parsedConfiguredOrigin.origin;
  } catch {
    return false;
  }
}

export function applyCors(req, res) {
  const origin = (req.headers || {}).origin;
  if (origin === undefined) return true;
  if (!isAllowedCorsOrigin(req)) {
    res.status(403).json({ ok: false, error: "Request origin rejected." });
    return false;
  }

  res.setHeader("Access-Control-Allow-Origin", origin);
  res.setHeader("Access-Control-Allow-Credentials", "true");
  res.setHeader("Access-Control-Allow-Methods", "GET, POST, DELETE, OPTIONS");
  res.setHeader("Access-Control-Allow-Headers", "Content-Type");
  res.setHeader("Access-Control-Max-Age", "600");
  res.setHeader("Vary", "Origin");

  if (req.method === "OPTIONS") {
    res.status(204).end();
    return false;
  }
  return true;
}
