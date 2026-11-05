import assert from "node:assert/strict";
import test from "node:test";
import controlHandler from "../api/control.js";
import realtimeConfigHandler from "../api/realtime-config.js";
import statusHandler from "../api/status.js";
import telemetryHandler from "../api/telemetry.js";
import { createControlSession } from "../lib/control-auth.js";

const serviceUrl = "https://secure-project.supabase.co";
const serviceKey = "test-service-role-key";
const controlPassword = "a-private-control-passphrase-with-more-than-16-characters";
const validTelemetry = {
  status: "safe",
  distanceCm: 15,
  ultrasonicOk: true,
  levelPercent: 0,
  waterHeightCm: 0,
  ledOn: false,
  green: true,
  yellow: false,
  red: false,
  buzzer: false,
  buzzerEnabled: true,
  buzzerMode: "Active HIGH",
  buzzerType: "active",
  buzzerTest: false,
  changeAlertCm: 1,
  lastAlertDeltaCm: 2,
  lastAlertDistanceCm: 15,
  lastAlertDirection: "Rising water",
  lastAlertAgeSec: 1,
  distanceChangeAlertCount: 0,
  clients: 1,
  uptime: 10,
  commandAck: "",
};

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

function setEnvironment(values) {
  const previous = new Map(Object.keys(values).map(key => [key, process.env[key]]));
  Object.assign(process.env, values);
  return () => {
    for (const [key, value] of previous) {
      if (value === undefined) delete process.env[key];
      else process.env[key] = value;
    }
  };
}

function mockSupabase(handler) {
  const originalFetch = globalThis.fetch;
  const requests = [];
  globalThis.fetch = async (input, init = {}) => {
    const url = new URL(input);
    requests.push({ url, init });
    const result = await handler(url, init);
    const status = result.status ?? 200;
    return new Response(status === 204 ? null : result.body ?? "", {
      status,
      headers: { "Content-Type": "application/json" },
    });
  };
  return {
    requests,
    restore() {
      globalThis.fetch = originalFetch;
    },
  };
}

test("status API returns the latest Supabase telemetry without caching", async () => {
  const restoreEnvironment = setEnvironment({
    SUPABASE_URL: serviceUrl,
    SUPABASE_SERVICE_ROLE_KEY: serviceKey,
  });
  const supabase = mockSupabase(() => ({
    body: JSON.stringify([{ data: { ...validTelemetry, receivedAt: new Date().toISOString() } }]),
  }));
  try {
    const response = createResponse();
    await statusHandler({ method: "GET", headers: {} }, response);

    assert.equal(response.statusCode, 200);
    assert.equal(response.body.available, true);
    assert.equal(response.body.distanceCm, 15);
    assert.equal(response.headers["Cache-Control"], "no-store");
    assert.equal(supabase.requests[0].url.pathname, "/rest/v1/secure_telemetry");
    assert.equal(supabase.requests[0].init.headers.apikey, serviceKey);
    assert.equal(supabase.requests[0].init.headers.Authorization, `Bearer ${serviceKey}`);
  } finally {
    supabase.restore();
    restoreEnvironment();
  }
});

test("status API hides telemetry after the device goes offline", async () => {
  const restoreEnvironment = setEnvironment({
    SUPABASE_URL: serviceUrl,
    SUPABASE_SERVICE_ROLE_KEY: serviceKey,
  });
  const supabase = mockSupabase(() => ({
    body: JSON.stringify([{ data: { ...validTelemetry, receivedAt: new Date(Date.now() - 90_000).toISOString() } }]),
  }));
  try {
    const response = createResponse();
    await statusHandler({ method: "GET", headers: {} }, response);

    assert.equal(response.statusCode, 200);
    assert.deepEqual(response.body, { ok: true, available: false });
  } finally {
    supabase.restore();
    restoreEnvironment();
  }
});

test("status API keeps fresh telemetry online when the writer clock is slightly ahead", async () => {
  const restoreEnvironment = setEnvironment({
    SUPABASE_URL: serviceUrl,
    SUPABASE_SERVICE_ROLE_KEY: serviceKey,
  });
  const receivedAt = new Date(Date.now() + 10_000).toISOString();
  const supabase = mockSupabase(() => ({
    body: JSON.stringify([{ data: { ...validTelemetry, receivedAt } }]),
  }));
  try {
    const response = createResponse();
    await statusHandler({ method: "GET", headers: {} }, response);

    assert.equal(response.statusCode, 200);
    assert.equal(response.body.available, true);
    assert.equal(response.body.receivedAt, receivedAt);
  } finally {
    supabase.restore();
    restoreEnvironment();
  }
});

test("realtime config exposes only the public key and Supabase URL", () => {
  const restoreEnvironment = setEnvironment({
    SUPABASE_URL: serviceUrl,
    SUPABASE_ANON_KEY: "test-publishable-key",
    SUPABASE_SERVICE_ROLE_KEY: serviceKey,
  });
  try {
    const response = createResponse();
    realtimeConfigHandler({ method: "GET" }, response);

    assert.equal(response.statusCode, 200);
    assert.equal(response.headers["Cache-Control"], "no-store");
    assert.deepEqual(response.body, {
      ok: true,
      enabled: true,
      url: serviceUrl,
      publishableKey: "test-publishable-key",
    });
    assert.equal(JSON.stringify(response.body).includes(serviceKey), false);
  } finally {
    restoreEnvironment();
  }
});

test("realtime config remains disabled until a public key is configured", () => {
  const restoreEnvironment = setEnvironment({ SUPABASE_URL: serviceUrl });
  const previousKey = process.env.SUPABASE_ANON_KEY;
  delete process.env.SUPABASE_ANON_KEY;
  try {
    const response = createResponse();
    realtimeConfigHandler({ method: "GET" }, response);

    assert.equal(response.statusCode, 200);
    assert.deepEqual(response.body, { ok: true, enabled: false });
  } finally {
    if (previousKey === undefined) delete process.env.SUPABASE_ANON_KEY;
    else process.env.SUPABASE_ANON_KEY = previousKey;
    restoreEnvironment();
  }
});

test("telemetry API broadcasts accepted readings to private Realtime", async () => {
  const restoreEnvironment = setEnvironment({
    AQUAGUARD_DEVICE_TOKEN: "a-valid-device-token",
    SUPABASE_URL: serviceUrl,
    SUPABASE_SERVICE_ROLE_KEY: serviceKey,
    SUPABASE_ANON_KEY: "test-publishable-key",
  });
  const supabase = mockSupabase(url => url.pathname.includes("/realtime/v1/")
    ? { status: 200 }
    : url.pathname.endsWith("/secure_control_command")
      ? { body: "[]" }
      : { status: 201 });
  try {
    const response = createResponse();
    await telemetryHandler({
      method: "POST",
      headers: { authorization: "Bearer a-valid-device-token" },
      body: validTelemetry,
    }, response);

    assert.equal(response.statusCode, 200);
    const broadcast = supabase.requests.find(request =>
      request.url.pathname === "/realtime/v1/api/broadcast/secure-telemetry/events/reading"
    );
    assert.ok(broadcast);
    assert.equal(broadcast.url.searchParams.get("private"), "true");
    assert.equal(broadcast.init.headers.apikey, serviceKey);
    assert.deepEqual(JSON.parse(broadcast.init.body).telemetry, {
      ...validTelemetry,
      receivedAt: JSON.parse(broadcast.init.body).telemetry.receivedAt,
    });
  } finally {
    supabase.restore();
    restoreEnvironment();
  }
});

test("telemetry API saves hardware readings and returns a pending device command", async () => {
  const restoreEnvironment = setEnvironment({
    AQUAGUARD_DEVICE_TOKEN: "a-valid-device-token",
    SUPABASE_URL: serviceUrl,
    SUPABASE_SERVICE_ROLE_KEY: serviceKey,
    SUPABASE_ANON_KEY: "test-publishable-key",
  });
  const commandId = "123e4567-e89b-42d3-a456-426614174000";
  const supabase = mockSupabase(url => {
    if (url.pathname.endsWith("/secure_control_command")) {
      return {
        body: JSON.stringify([{
          command_id: commandId,
          action: "off",
          target: "buzzer",
          expires_at: new Date(Date.now() + 30_000).toISOString(),
        }]),
      };
    }
    return { status: 201 };
  });
  try {
    const response = createResponse();
    await telemetryHandler({
      method: "POST",
      headers: { authorization: "Bearer a-valid-device-token" },
      body: validTelemetry,
    }, response);

    assert.equal(response.statusCode, 200);
    assert.deepEqual(response.body.command, { id: commandId, action: "off", target: "buzzer" });
    assert.equal(supabase.requests[0].url.pathname, "/rest/v1/secure_telemetry");
    assert.equal(supabase.requests[0].init.method, "POST");
    const broadcast = supabase.requests.find(request =>
      request.url.pathname === "/realtime/v1/api/broadcast/secure-telemetry/events/reading"
    );
    assert.ok(broadcast);
    assert.equal(broadcast.url.searchParams.get("private"), "true");
    assert.equal(broadcast.init.headers.apikey, serviceKey);
    assert.deepEqual(JSON.parse(broadcast.init.body), {
      telemetry: { ...validTelemetry, receivedAt: JSON.parse(broadcast.init.body).telemetry.receivedAt },
    });
    assert.ok(supabase.requests.some(request =>
      request.url.pathname === "/rest/v1/secure_control_command"
    ));
  } finally {
    supabase.restore();
    restoreEnvironment();
  }
});

test("telemetry remains accepted when the optional realtime broadcast fails", async () => {
  const restoreEnvironment = setEnvironment({
    AQUAGUARD_DEVICE_TOKEN: "a-valid-device-token",
    SUPABASE_URL: serviceUrl,
    SUPABASE_SERVICE_ROLE_KEY: serviceKey,
    SUPABASE_ANON_KEY: "test-publishable-key",
  });
  const supabase = mockSupabase(url => url.pathname.includes("/realtime/v1/")
    ? { status: 503 }
    : url.pathname.endsWith("/secure_control_command")
      ? { body: "[]" }
      : { status: 201 });
  const originalConsoleError = console.error;
  const errors = [];
  console.error = (...args) => errors.push(args.join(" "));
  try {
    const response = createResponse();
    await telemetryHandler({
      method: "POST",
      headers: { authorization: ["Bearer", "a-valid-device-token"].join(" ") },
      body: validTelemetry,
    }, response);

    assert.equal(response.statusCode, 200);
    assert.match(errors[0], /Could not broadcast the latest telemetry/);
  } finally {
    console.error = originalConsoleError;
    supabase.restore();
    restoreEnvironment();
  }
});

test("telemetry API returns a pending change-alert setting to the ESP32", async () => {
  const restoreEnvironment = setEnvironment({
    AQUAGUARD_DEVICE_TOKEN: "a-valid-device-token",
    SUPABASE_URL: serviceUrl,
    SUPABASE_SERVICE_ROLE_KEY: serviceKey,
  });
  const commandId = "123e4567-e89b-42d3-a456-426614174000";
  const supabase = mockSupabase(url => url.pathname.endsWith("/secure_control_command")
    ? {
      body: JSON.stringify([{
        command_id: commandId,
        action: "set-change-alert",
        target: "settings",
        value: 3,
        expires_at: new Date(Date.now() + 30_000).toISOString(),
      }]),
    }
    : { status: 201 });
  try {
    const response = createResponse();
    await telemetryHandler({
      method: "POST",
      headers: { authorization: `Bearer ${process.env.AQUAGUARD_DEVICE_TOKEN}` },
      body: validTelemetry,
    }, response);

    assert.equal(response.statusCode, 200);
    assert.deepEqual(response.body.command, {
      id: commandId,
      action: "set-change-alert",
      target: "settings",
      value: 3,
    });
  } finally {
    supabase.restore();
    restoreEnvironment();
  }
});

test("telemetry acknowledgement deletes only the matching Supabase command", async () => {
  const restoreEnvironment = setEnvironment({
    AQUAGUARD_DEVICE_TOKEN: "a-valid-device-token",
    SUPABASE_URL: serviceUrl,
    SUPABASE_SERVICE_ROLE_KEY: serviceKey,
  });
  const commandId = "123e4567-e89b-42d3-a456-426614174000";
  const supabase = mockSupabase(url => url.pathname.endsWith("/secure_control_command")
    ? {
      body: JSON.stringify([{
        command_id: commandId,
        action: "on",
        expires_at: new Date(Date.now() + 30_000).toISOString(),
      }]),
    }
    : { status: 204 });
  try {
    const response = createResponse();
    await telemetryHandler({
      method: "POST",
      headers: { authorization: "Bearer a-valid-device-token" },
      body: { ...validTelemetry, commandAck: commandId },
    }, response);

    assert.equal(response.statusCode, 200);
    assert.deepEqual(response.body, { ok: true });
    assert.equal(supabase.requests[2].init.method, "DELETE");
    assert.match(supabase.requests[2].url.search, new RegExp(`command_id=eq\\.${commandId}`));
  } finally {
    supabase.restore();
    restoreEnvironment();
  }
});

test("control API queues a signed-in command in Supabase", async () => {
  const restoreEnvironment = setEnvironment({
    AQUAGUARD_CONTROL_PASSWORD: controlPassword,
    SUPABASE_URL: serviceUrl,
    SUPABASE_SERVICE_ROLE_KEY: serviceKey,
  });
  const supabase = mockSupabase(() => ({ status: 201 }));
  try {
    const session = createControlSession(controlPassword);
    const response = createResponse();
    await controlHandler({
      method: "POST",
      headers: {
        origin: "https://secure-app-live.vercel.app",
        host: "secure-app-live.vercel.app",
        cookie: `__Secure-aquaguard_control=${session.value}`,
      },
      body: { action: "on" },
    }, response);

    assert.equal(response.statusCode, 202);
    assert.equal(response.body.ok, true);
    const queued = JSON.parse(supabase.requests[0].init.body)[0];
    assert.equal(queued.action, "on");
    assert.equal(queued.target, "led");
    assert.equal(queued.singleton, true);
    assert.equal(queued.command_id, response.body.commandId);
  } finally {
    supabase.restore();
    restoreEnvironment();
  }
});

test("control API queues a buzzer command for the signed-in device", async () => {
  const restoreEnvironment = setEnvironment({
    AQUAGUARD_CONTROL_PASSWORD: controlPassword,
    SUPABASE_URL: serviceUrl,
    SUPABASE_SERVICE_ROLE_KEY: serviceKey,
  });
  const supabase = mockSupabase(() => ({ status: 201 }));
  try {
    const session = createControlSession(controlPassword);
    const response = createResponse();
    await controlHandler({
      method: "POST",
      headers: {
        origin: "https://secure-app-live.vercel.app",
        host: "secure-app-live.vercel.app",
        cookie: `__Secure-aquaguard_control=${session.value}`,
      },
      body: { action: "off", target: "buzzer" },
    }, response);

    assert.equal(response.statusCode, 202);
    assert.equal(response.body.target, "buzzer");
    const queued = JSON.parse(supabase.requests[0].init.body)[0];
    assert.equal(queued.action, "off");
    assert.equal(queued.target, "buzzer");
  } finally {
    supabase.restore();
    restoreEnvironment();
  }
});

test("control API queues a validated change-alert setting for the signed-in device", async () => {
  const restoreEnvironment = setEnvironment({
    AQUAGUARD_CONTROL_PASSWORD: controlPassword,
    SUPABASE_URL: serviceUrl,
    SUPABASE_SERVICE_ROLE_KEY: serviceKey,
  });
  const supabase = mockSupabase(() => ({ status: 201 }));
  try {
    const session = createControlSession(controlPassword);
    const response = createResponse();
    await controlHandler({
      method: "POST",
      headers: {
        origin: "https://secure-app-live.vercel.app",
        host: "secure-app-live.vercel.app",
        cookie: `__Secure-aquaguard_control=${session.value}`,
      },
      body: { action: "set-change-alert", target: "settings", value: 3 },
    }, response);

    assert.equal(response.statusCode, 202);
    assert.deepEqual(response.body, {
      ok: true,
      commandId: response.body.commandId,
      target: "settings",
      value: 3,
    });
    assert.match(response.body.commandId, /^[0-9a-f-]{36}$/i);
    const queued = JSON.parse(supabase.requests[0].init.body)[0];
    assert.equal(queued.action, "set-change-alert");
    assert.equal(queued.target, "settings");
    assert.equal(queued.value, 3);
  } finally {
    supabase.restore();
    restoreEnvironment();
  }
});

test("control API rejects out-of-range or fractional-step change-alert settings", async () => {
  const restoreEnvironment = setEnvironment({
    AQUAGUARD_CONTROL_PASSWORD: controlPassword,
    SUPABASE_URL: serviceUrl,
    SUPABASE_SERVICE_ROLE_KEY: serviceKey,
  });
  const supabase = mockSupabase(() => ({ status: 201 }));
  try {
    const session = createControlSession(controlPassword);
    for (const value of [0.4, 20.5, 1.25, "3"]) {
      const response = createResponse();
      await controlHandler({
        method: "POST",
        headers: {
          origin: "https://secure-app-live.vercel.app",
          host: "secure-app-live.vercel.app",
          cookie: `__Secure-aquaguard_control=${session.value}`,
        },
        body: { action: "set-change-alert", target: "settings", value },
      }, response);
      assert.equal(response.statusCode, 400);
    }
    assert.equal(supabase.requests.length, 0);
  } finally {
    supabase.restore();
    restoreEnvironment();
  }
});

test("missing Supabase configuration fails explicitly instead of returning empty readings", async () => {
  const restoreEnvironment = setEnvironment({});
  const previousUrl = process.env.SUPABASE_URL;
  const previousKey = process.env.SUPABASE_SERVICE_ROLE_KEY;
  const originalConsoleError = console.error;
  delete process.env.SUPABASE_URL;
  delete process.env.SUPABASE_SERVICE_ROLE_KEY;
  console.error = () => {};
  try {
    const response = createResponse();
    await statusHandler({ method: "GET", headers: {} }, response);

    assert.equal(response.statusCode, 503);
    assert.equal(response.body.available, undefined);
  } finally {
    console.error = originalConsoleError;
    if (previousUrl === undefined) delete process.env.SUPABASE_URL;
    else process.env.SUPABASE_URL = previousUrl;
    if (previousKey === undefined) delete process.env.SUPABASE_SERVICE_ROLE_KEY;
    else process.env.SUPABASE_SERVICE_ROLE_KEY = previousKey;
    restoreEnvironment();
  }
});

test("telemetry API includes safe PostgREST diagnostics when command reads fail", async () => {
  const restoreEnvironment = setEnvironment({
    AQUAGUARD_DEVICE_TOKEN: "a-valid-device-token",
    SUPABASE_URL: serviceUrl,
    SUPABASE_SERVICE_ROLE_KEY: serviceKey,
  });
  const supabase = mockSupabase(url => url.pathname.endsWith("/secure_control_command")
    ? {
      status: 400,
      body: JSON.stringify({
        code: "PGRST204",
        message: "Could not find the target column.",
        details: "Sensitive row detail",
      }),
    }
    : { status: 201 });
  const originalConsoleError = console.error;
  const errors = [];
  console.error = (...args) => errors.push(args.join(" "));
  try {
    const response = createResponse();
    await telemetryHandler({
      method: "POST",
      headers: { authorization: `Bearer ${"a-valid-device-token"}` },
      body: validTelemetry,
    }, response);

    assert.equal(response.statusCode, 503);
    assert.match(errors[0], /PGRST204: Could not find the target column/);
    assert.doesNotMatch(errors[0], /Sensitive row detail/);
  } finally {
    console.error = originalConsoleError;
    supabase.restore();
    restoreEnvironment();
  }
});
