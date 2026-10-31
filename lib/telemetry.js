import { timingSafeEqual } from "node:crypto";

const STATUSES = new Set(["safe", "warning", "critical", "fault"]);

export function isAuthorizedAuthorizationHeader(header, expectedToken) {
  if (typeof header !== "string" || typeof expectedToken !== "string") {
    return false;
  }

  const [scheme, token, ...extra] = header.split(" ");
  if (
    scheme !== "Bearer" ||
    !token ||
    extra.length > 0 ||
    token.length !== expectedToken.length
  ) {
    return false;
  }

  return timingSafeEqual(Buffer.from(token), Buffer.from(expectedToken));
}

export function normalizeTelemetry(body, receivedAt) {
  if (!body || typeof body !== "object" || Array.isArray(body)) {
    return null;
  }

  const {
    status,
    distanceCm,
    ultrasonicOk,
    levelPercent,
    waterHeightCm,
    ledOn,
    green,
    yellow,
    red,
    buzzer,
    uptime,
  } = body;

  if (
    !STATUSES.has(status) ||
    typeof ultrasonicOk !== "boolean" ||
    typeof ledOn !== "boolean" ||
    typeof green !== "boolean" ||
    typeof yellow !== "boolean" ||
    typeof red !== "boolean" ||
    typeof buzzer !== "boolean" ||
    !isFiniteNumber(distanceCm, -1, 400) ||
    !isFiniteNumber(levelPercent, -1, 100) ||
    !isFiniteNumber(waterHeightCm, -1, 400) ||
    !isFiniteNumber(uptime, 0, Number.MAX_SAFE_INTEGER)
  ) {
    return null;
  }

  return {
    status,
    distanceCm,
    ultrasonicOk,
    levelPercent,
    waterHeightCm,
    ledOn,
    green,
    yellow,
    red,
    buzzer,
    uptime,
    receivedAt,
  };
}

function isFiniteNumber(value, min, max) {
  return typeof value === "number" && Number.isFinite(value) && value >= min && value <= max;
}
