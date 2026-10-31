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
    buzzerMode,
    buzzerType,
    buzzerTest,
    changeAlertCm,
    lastAlertDeltaCm,
    lastAlertDistanceCm,
    lastAlertDirection,
    lastAlertAgeSec,
    distanceChangeAlertCount,
    clients,
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
    typeof buzzerTest !== "boolean" ||
    !["Active HIGH", "Passive (PWM)"].includes(buzzerMode) ||
    !["active", "passive"].includes(buzzerType) ||
    !isFiniteNumber(distanceCm, -1, 400) ||
    !isFiniteNumber(levelPercent, -1, 100) ||
    !isFiniteNumber(waterHeightCm, -1, 400) ||
    !isFiniteNumber(changeAlertCm, 0.5, 20) ||
    !isFiniteNumber(lastAlertDeltaCm, -1, 400) ||
    !isFiniteNumber(lastAlertDistanceCm, -1, 400) ||
    !["Rising water", "Falling water"].includes(lastAlertDirection) ||
    !isFiniteNumber(lastAlertAgeSec, -1, Number.MAX_SAFE_INTEGER) ||
    !Number.isSafeInteger(distanceChangeAlertCount) ||
    distanceChangeAlertCount < 0 ||
    !Number.isInteger(clients) ||
    clients < 0 ||
    clients > 255 ||
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
    buzzerMode,
    buzzerType,
    buzzerTest,
    changeAlertCm,
    lastAlertDeltaCm,
    lastAlertDistanceCm,
    lastAlertDirection,
    lastAlertAgeSec,
    distanceChangeAlertCount,
    clients,
    uptime,
    receivedAt,
  };
}

function isFiniteNumber(value, min, max) {
  return typeof value === "number" && Number.isFinite(value) && value >= min && value <= max;
}
