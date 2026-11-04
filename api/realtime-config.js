export default function handler(req, res) {
  res.setHeader("Cache-Control", "no-store");

  if (req.method !== "GET") {
    res.setHeader("Allow", "GET");
    return res.status(405).json({ ok: false, error: "Method not allowed." });
  }

  const rawUrl = process.env.SUPABASE_URL;
  const publishableKey = process.env.SUPABASE_ANON_KEY;
  if (!rawUrl || !publishableKey) {
    return res.status(200).json({ ok: true, enabled: false });
  }

  let url;
  try {
    url = new URL(rawUrl);
  } catch {
    console.error("SUPABASE_URL is invalid; live telemetry broadcasts are disabled.");
    return res.status(500).json({ ok: false, error: "Realtime service is not configured." });
  }
  if (url.protocol !== "https:" || url.username || url.password) {
    console.error("SUPABASE_URL must be an HTTPS URL; live telemetry broadcasts are disabled.");
    return res.status(500).json({ ok: false, error: "Realtime service is not configured." });
  }

  return res.status(200).json({
    ok: true,
    enabled: true,
    url: url.origin,
    publishableKey,
  });
}
