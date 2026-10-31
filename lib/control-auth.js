import { createHmac, timingSafeEqual } from "node:crypto";

export const CONTROL_COOKIE_NAME = "__Secure-aquaguard_control";
export const CONTROL_SESSION_MAX_AGE_SECONDS = 8 * 60 * 60;
export const MIN_CONTROL_PASSWORD_LENGTH = 16;

export function isValidControlPassword(password, expectedPassword) {
  if (
    typeof password !== "string" ||
    typeof expectedPassword !== "string" ||
    Buffer.byteLength(expectedPassword) < MIN_CONTROL_PASSWORD_LENGTH
  ) {
    return false;
  }

  const provided = Buffer.from(password);
  const expected = Buffer.from(expectedPassword);
  return provided.length === expected.length && timingSafeEqual(provided, expected);
}

export function createControlSession(password, now = Date.now()) {
  const expiresAt = Math.floor(now / 1000) + CONTROL_SESSION_MAX_AGE_SECONDS;
  const signature = signSession(password, expiresAt);
  return { value: `${expiresAt}.${signature}`, expiresAt };
}

export function isAuthorizedControlCookie(cookieHeader, password, now = Date.now()) {
  if (
    typeof cookieHeader !== "string" ||
    typeof password !== "string" ||
    Buffer.byteLength(password) < MIN_CONTROL_PASSWORD_LENGTH
  ) {
    return false;
  }

  const cookie = cookieHeader
    .split(";")
    .map((part) => part.trim())
    .find((part) => part.startsWith(`${CONTROL_COOKIE_NAME}=`));

  if (!cookie) return false;

  const [expiresText, signature, ...extra] = cookie
    .slice(CONTROL_COOKIE_NAME.length + 1)
    .split(".");
  if (
    extra.length > 0 ||
    !/^\d{10}$/.test(expiresText) ||
    !/^[0-9a-f]{64}$/.test(signature)
  ) {
    return false;
  }

  const expiresAt = Number(expiresText);
  const nowSeconds = Math.floor(now / 1000);
  if (
    expiresAt <= nowSeconds ||
    expiresAt > nowSeconds + CONTROL_SESSION_MAX_AGE_SECONDS + 60
  ) {
    return false;
  }

  const expected = Buffer.from(signSession(password, expiresAt), "hex");
  const provided = Buffer.from(signature, "hex");
  return timingSafeEqual(provided, expected);
}

export function isSameOriginRequest(req) {
  const origin = req.headers.origin;
  const host = req.headers["x-forwarded-host"] || req.headers.host;
  if (typeof origin !== "string" || typeof host !== "string") return false;

  try {
    const parsedOrigin = new URL(origin);
    return parsedOrigin.protocol === "https:" && parsedOrigin.host === host;
  } catch {
    return false;
  }
}

function signSession(password, expiresAt) {
  return createHmac("sha256", password).update(String(expiresAt)).digest("hex");
}
