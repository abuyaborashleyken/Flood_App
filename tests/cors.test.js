import test from "node:test";
import assert from "node:assert/strict";
import { isAllowedCorsOrigin } from "../lib/cors.js";

test("accepts same-origin HTTPS requests", () => {
  assert.equal(
    isAllowedCorsOrigin({
      headers: {
        origin: "https://secure-app-live.vercel.app",
        host: "secure-app-live.vercel.app",
      },
    }),
    true,
  );
});

test("accepts only the configured HTTPS frontend origin", () => {
  const previousOrigin = process.env.AQUAGUARD_APP_ORIGIN;
  process.env.AQUAGUARD_APP_ORIGIN = "https://dashboard.infinityfreeapp.com";
  try {
    const request = {
      headers: {
        origin: "https://dashboard.infinityfreeapp.com",
        host: "secure-app-live.vercel.app",
      },
    };
    assert.equal(isAllowedCorsOrigin(request), true);
    assert.equal(
      isAllowedCorsOrigin({
        ...request,
        headers: { ...request.headers, origin: "https://attacker.test" },
      }),
      false,
    );
  } finally {
    if (previousOrigin === undefined) delete process.env.AQUAGUARD_APP_ORIGIN;
    else process.env.AQUAGUARD_APP_ORIGIN = previousOrigin;
  }
});

test("rejects insecure, malformed, and unconfigured frontend origins", () => {
  const previousOrigin = process.env.AQUAGUARD_APP_ORIGIN;
  delete process.env.AQUAGUARD_APP_ORIGIN;
  try {
    assert.equal(
      isAllowedCorsOrigin({
        headers: { origin: "http://dashboard.infinityfreeapp.com", host: "api.test" },
      }),
      false,
    );
    assert.equal(
      isAllowedCorsOrigin({
        headers: { origin: "https://dashboard.infinityfreeapp.com/path", host: "api.test" },
      }),
      false,
    );
  } finally {
    if (previousOrigin === undefined) delete process.env.AQUAGUARD_APP_ORIGIN;
    else process.env.AQUAGUARD_APP_ORIGIN = previousOrigin;
  }
});
