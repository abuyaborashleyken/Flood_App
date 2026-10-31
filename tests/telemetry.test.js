import test from "node:test";
import assert from "node:assert/strict";
import {
  isAuthorizedAuthorizationHeader,
  normalizeTelemetry,
} from "../lib/telemetry.js";

const validPayload = {
  status: "warning",
  distanceCm: 9.5,
  ultrasonicOk: true,
  levelPercent: 36.7,
  waterHeightCm: 5.5,
  ledOn: true,
  green: false,
  yellow: true,
  red: false,
  buzzer: true,
  buzzerMode: "Active HIGH",
  buzzerType: "active",
  buzzerTest: false,
  changeAlertCm: 1,
  lastAlertDeltaCm: 1.5,
  lastAlertDistanceCm: 9.5,
  lastAlertDirection: "Rising water",
  lastAlertAgeSec: 2,
  distanceChangeAlertCount: 4,
  clients: 1,
  uptime: 1200,
};

test("accepts a correctly authenticated device", () => {
  assert.equal(isAuthorizedAuthorizationHeader("Bearer abc123", "abc123"), true);
});

test("rejects malformed or incorrect authorization headers", () => {
  assert.equal(isAuthorizedAuthorizationHeader("Basic abc123", "abc123"), false);
  assert.equal(isAuthorizedAuthorizationHeader("Bearer abc124", "abc123"), false);
  assert.equal(isAuthorizedAuthorizationHeader("Bearer abc123 extra", "abc123"), false);
});

test("normalizes valid telemetry and adds server receipt time", () => {
  const normalized = normalizeTelemetry(validPayload, "2026-10-31T12:00:00.000Z");
  assert.deepEqual(normalized, {
    ...validPayload,
    receivedAt: "2026-10-31T12:00:00.000Z",
  });
});

test("rejects malformed or out-of-range telemetry", () => {
  assert.equal(normalizeTelemetry(null, "now"), null);
  assert.equal(normalizeTelemetry({ ...validPayload, distanceCm: 401 }, "now"), null);
  assert.equal(normalizeTelemetry({ ...validPayload, ultrasonicOk: 1 }, "now"), null);
  assert.equal(normalizeTelemetry({ ...validPayload, status: "unknown" }, "now"), null);
});
