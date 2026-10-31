import { get } from "@vercel/blob";

export default async function handler(req, res) {
  res.setHeader("Cache-Control", "no-store");

  if (req.method !== "GET") {
    res.setHeader("Allow", "GET");
    return res.status(405).json({ ok: false, error: "Method not allowed." });
  }

  try {
    const blob = await get("telemetry/latest.json", {
      access: "private",
      useCache: false,
    });

    if (!blob) {
      return res.status(200).json({ ok: true, available: false });
    }

    const telemetry = JSON.parse(await new Response(blob.stream).text());
    return res.status(200).json({ ok: true, available: true, ...telemetry });
  } catch (error) {
    console.error("Could not read AquaGuard telemetry.", error);
    return res.status(503).json({ ok: false, error: "Could not load telemetry." });
  }
}
