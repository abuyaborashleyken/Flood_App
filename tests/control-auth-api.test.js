import test from "node:test";
import assert from "node:assert/strict";
import handler from "../api/control-auth.js";
import {
  CONTROL_COOKIE_NAME,
  isAuthorizedControlCookie,
} from "../lib/control-auth.js";

const password = "a-private-control-passphrase-with-more-than-16-characters";

function createResponse() {
  return {
    headers: {},
    statusCode: 200,
    setHeader(name, value) {
      this.headers[name] = value;
    },
    status(code) {
      this.statusCode = code;
      return this;
    },
    json(body) {
      this.body = body;
      return this;
    },
  };
}

test("issues an HttpOnly, secure, same-site session for the correct password", () => {
  const previousPassword = process.env.AQUAGUARD_CONTROL_PASSWORD;
  process.env.AQUAGUARD_CONTROL_PASSWORD = password;

  try {
    const response = createResponse();
    handler(
      {
        method: "POST",
        headers: { origin: "https://aquaguard.test", host: "aquaguard.test" },
        body: { password },
      },
      response,
    );

    assert.equal(response.statusCode, 200);
    assert.equal(response.body.authenticated, true);
    assert.match(response.headers["Set-Cookie"], /HttpOnly; Secure; SameSite=Strict/);
    const cookie = response.headers["Set-Cookie"].split(";")[0];
    assert.equal(
      isAuthorizedControlCookie(cookie, password),
      true,
    );
    assert.equal(cookie.startsWith(`${CONTROL_COOKIE_NAME}=`), true);
  } finally {
    if (previousPassword === undefined) delete process.env.AQUAGUARD_CONTROL_PASSWORD;
    else process.env.AQUAGUARD_CONTROL_PASSWORD = previousPassword;
  }
});

test("rejects incorrect passwords and cross-origin logins", () => {
  const previousPassword = process.env.AQUAGUARD_CONTROL_PASSWORD;
  process.env.AQUAGUARD_CONTROL_PASSWORD = password;

  try {
    const incorrect = createResponse();
    handler(
      {
        method: "POST",
        headers: { origin: "https://aquaguard.test", host: "aquaguard.test" },
        body: { password: "incorrect-control-password" },
      },
      incorrect,
    );
    assert.equal(incorrect.statusCode, 401);

    const crossOrigin = createResponse();
    handler(
      {
        method: "POST",
        headers: { origin: "https://attacker.test", host: "aquaguard.test" },
        body: { password },
      },
      crossOrigin,
    );
    assert.equal(crossOrigin.statusCode, 403);
  } finally {
    if (previousPassword === undefined) delete process.env.AQUAGUARD_CONTROL_PASSWORD;
    else process.env.AQUAGUARD_CONTROL_PASSWORD = previousPassword;
  }
});
