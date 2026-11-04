import { timingSafeEqual } from "node:crypto";

const STATUSES = new Set(["safe", "warning", "critical", "fault"]);
export const TELEMETRY_TIMEOUT_MS = 90_000;
export const TELEMETRY_CLOCK_SKEW_MS = 30_000;

export function isTelemetryFresh(receivedAt, now = Date.now()) {
  const timestamp = typeof receivedAt === "number" ? receivedAt : Date.parse(receivedAt);
  const age = now - timestamp;
  return Number.isFinite(timestamp) &&
    Number.isFinite(now) &&
    age >= -TELEMETRY_CLOCK_SKEW_MS &&
    age < TELEMETRY_TIMEOUT_MS;
}

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
  const commandAck = body.commandAck ?? "";

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
    !isFiniteNumber(uptime, 0, Number.MAX_SAFE_INTEGER) ||
    typeof commandAck !== "string" ||
    (commandAck !== "" &&
      !/^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/i.test(
        commandAck,
      ))
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
    commandAck,
    receivedAt,
  };
}

function isFiniteNumber(value, min, max) {
  return typeof value === "number" && Number.isFinite(value) && value >= min && value <= max;
}
