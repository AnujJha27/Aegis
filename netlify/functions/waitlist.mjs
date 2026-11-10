// Aegis waitlist — Netlify function (Web-standard, v2 runtime).
// Storage: Netlify Blobs — a single "signups" blob holding the list.
// Robust across @netlify/blobs versions (no list() pagination API used).
// Signups: GET /api/waitlist/list?key=ADMIN_KEY

const EMAIL_RE = /^[^@\s]+@[^@\s]+\.[^@\s]+$/;
const json = (status, obj) =>
  new Response(JSON.stringify(obj), {
    status,
    headers: { "content-type": "application/json" },
  });

async function getStoreSafe() {
  const { getStore } = await import("@netlify/blobs");
  return getStore({ name: "waitlist", consistency: "strong" });
}

async function readEntries(store) {
  try {
    const raw = await store.get("signups");
    const parsed = raw ? JSON.parse(raw) : [];
    return Array.isArray(parsed) ? parsed : [];
  } catch {
    return [];
  }
}

export default async (req) => {
  const url = new URL(req.url);

  if (req.method === "GET" && url.pathname.endsWith("/health")) {
    return json(200, { ok: true, service: "aegis-waitlist" });
  }

  if (req.method === "GET" && url.pathname.endsWith("/list")) {
    const key = url.searchParams.get("key") || "";
    if (!process.env.ADMIN_KEY || key !== process.env.ADMIN_KEY) {
      return json(403, { ok: false, error: "Forbidden" });
    }
    const store = await getStoreSafe();
    const entries = await readEntries(store);
    return json(200, { ok: true, count: entries.length, signups: entries });
  }

  if (req.method !== "POST") {
    return json(405, { ok: false, error: "Method not allowed" });
  }

  let data = {};
  try { data = await req.json(); } catch {}
  if (String(data.website || "").trim()) {
    return json(200, { ok: true });          // honeypot — accept, store nothing
  }

  const email = String(data.email || "").trim().toLowerCase();
  if (!EMAIL_RE.test(email) || email.length > 254) {
    return json(400, { ok: false, error: "Enter a valid email address." });
  }

  try {
    const store = await getStoreSafe();
    const entries = await readEntries(store);
    const existing = entries.findIndex((e) => e.email === email);
    if (existing > -1) {
      return json(200, { ok: true, duplicate: true, position: existing + 1 });
    }
    entries.push({ email, ts: Date.now() });
    await store.setJSON("signups", entries);
    return json(200, { ok: true, position: entries.length });
  } catch (err) {
    return json(500, { ok: false, error: "Store unavailable." });
  }
};

export const config = { path: ["/api/waitlist", "/api/health", "/api/waitlist/list"] };
