const TELEMETRY_TABLE = "secure_telemetry";
const COMMAND_TABLE = "secure_control_command";

export async function saveTelemetry(telemetry, env = process.env) {
  await request(
    `${TELEMETRY_TABLE}?on_conflict=singleton`,
    {
      method: "POST",
      body: [{ singleton: true, data: telemetry }],
      prefer: "resolution=merge-duplicates,return=minimal",
    },
    env,
  );
}

export async function readTelemetry(env = process.env) {
  const rows = await request(
    `${TELEMETRY_TABLE}?select=data&singleton=eq.true&limit=1`,
    {},
    env,
  );
  return Array.isArray(rows) ? rows[0]?.data ?? null : null;
}

export async function queueControlCommand(command, env = process.env) {
  await request(
    `${COMMAND_TABLE}?on_conflict=singleton`,
    {
      method: "POST",
      body: [{
        singleton: true,
        command_id: command.id,
        action: command.action,
        created_at: command.createdAt,
        expires_at: new Date(command.expiresAt).toISOString(),
      }],
      prefer: "resolution=merge-duplicates,return=minimal",
    },
    env,
  );
}

export async function readPendingCommand(env = process.env) {
  const rows = await request(
    `${COMMAND_TABLE}?select=command_id,action,expires_at&singleton=eq.true&limit=1`,
    {},
    env,
  );
  if (!Array.isArray(rows) || !rows[0]) return null;

  const row = rows[0];
  return {
    id: row.command_id,
    action: row.action,
    expiresAt: Date.parse(row.expires_at),
  };
}

export async function clearPendingCommand(commandId, env = process.env) {
  await request(
    `${COMMAND_TABLE}?singleton=eq.true&command_id=eq.${encodeURIComponent(commandId)}`,
    { method: "DELETE", prefer: "return=minimal" },
    env,
  );
}

async function request(path, { method = "GET", body, prefer } = {}, env = process.env) {
  const config = getConfig(env);
  const headers = {
    apikey: config.serviceRoleKey,
    Authorization: `Bearer ${config.serviceRoleKey}`,
    Accept: "application/json",
  };
  if (body !== undefined) headers["Content-Type"] = "application/json";
  if (prefer) headers.Prefer = prefer;

  const response = await fetch(`${config.url}/rest/v1/${path}`, {
    method,
    headers,
    body: body === undefined ? undefined : JSON.stringify(body),
    cache: "no-store",
  });
  if (!response.ok) {
    throw new Error(`Supabase request failed with HTTP ${response.status}.`);
  }
  if (response.status === 204) return null;

  const responseText = await response.text();
  return responseText ? JSON.parse(responseText) : null;
}

function getConfig(env) {
  const rawUrl = env.SUPABASE_URL;
  const serviceRoleKey = env.SUPABASE_SERVICE_ROLE_KEY;
  if (typeof rawUrl !== "string" || typeof serviceRoleKey !== "string" || !serviceRoleKey) {
    throw new Error("Supabase is not configured. Set SUPABASE_URL and SUPABASE_SERVICE_ROLE_KEY.");
  }

  let url;
  try {
    url = new URL(rawUrl);
  } catch {
    throw new Error("SUPABASE_URL must be a valid HTTPS URL.");
  }
  if (url.protocol !== "https:" || url.username || url.password) {
    throw new Error("SUPABASE_URL must be a valid HTTPS URL.");
  }

  return { url: url.origin, serviceRoleKey };
}
