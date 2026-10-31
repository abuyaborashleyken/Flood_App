import test from "node:test";
import assert from "node:assert/strict";
import {
  CONTROL_COOKIE_NAME,
  CONTROL_SESSION_MAX_AGE_SECONDS,
  createControlSession,
  isAuthorizedControlCookie,
  isSameOriginRequest,
  isValidControlPassword,
} from "../lib/control-auth.js";

const password = "a-private-control-passphrase-with-more-than-16-characters";
const now = Date.UTC(2026, 9, 31, 12, 0, 0);

test("validates control passwords without accepting short values", () => {
  assert.equal(isValidControlPassword(password, password), true);
  assert.equal(isValidControlPassword("incorrect-password", password), false);
  assert.equal(isValidControlPassword("short", "short"), false);
  assert.equal(isValidControlPassword(password, undefined), false);
});

test("authenticates unexpired signed control cookies", () => {
  const session = createControlSession(password, now);
  const cookie = `${CONTROL_COOKIE_NAME}=${session.value}`;

  assert.equal(isAuthorizedControlCookie(cookie, password, now), true);
  assert.equal(isAuthorizedControlCookie(cookie, "a-different-control-password", now), false);
  assert.equal(isAuthorizedControlCookie(cookie.replace(/\.[0-9a-f]{64}$/, ".invalid"), password, now), false);
});

test("rejects expired, malformed, and unrelated cookies", () => {
  const expiredAt = now - (CONTROL_SESSION_MAX_AGE_SECONDS + 1) * 1000;
  const expiredSession = createControlSession(password, expiredAt);
  assert.equal(
    isAuthorizedControlCookie(`${CONTROL_COOKIE_NAME}=${expiredSession.value}`, password, now),
    false,
  );
  assert.equal(isAuthorizedControlCookie("other=value", password, now), false);
  assert.equal(
    isAuthorizedControlCookie(`${CONTROL_COOKIE_NAME}=not-a-session`, password, now),
    false,
  );
});

test("accepts only HTTPS requests from the same origin", () => {
  assert.equal(
    isSameOriginRequest({
      headers: { origin: "https://aquaguard.test", host: "aquaguard.test" },
    }),
    true,
  );
  assert.equal(
    isSameOriginRequest({
      headers: { origin: "https://attacker.test", host: "aquaguard.test" },
    }),
    false,
  );
  assert.equal(
    isSameOriginRequest({
      headers: { origin: "http://aquaguard.test", host: "aquaguard.test" },
    }),
    false,
  );
});
