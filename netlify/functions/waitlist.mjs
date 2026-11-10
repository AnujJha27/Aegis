// Aegis waitlist — Netlify function (Web-standard, v2 runtime).
// Storage: Netlify Blobs — one blob per email (signup:<email>), plus a
// read-only legacy "signups" blob checked for duplicates (never rewritten).
// Admin export: GET /api/waitlist/list?key=ADMIN_KEY (paginated list).

const EMAIL_RE = /^[^@\s]+@[^@\s]+\.[^@\s]+$/;
const json = (status, obj) =>
  new Response(JSON.stringify(obj), {
    status,
    headers: { "content-type": "application/json" },
  });

const LEGACY_KEY = "signups";
const keyFor = (email) => `signup:${email}`;
const normalize = (e) => String(e || "").trim().toLowerCase();

async function getStoreSafe() {
  const { getStore } = await import("@netlify/blobs");
  return getStore({ name: "waitlist", consistency: "strong" });
}

// Strict legacy read: parse/shape failures throw (mapped to 503).
// The legacy blob is never rewritten, migrated, or deleted.
async function readLegacy(store) {
  const raw = await store.get(LEGACY_KEY);
  if (!raw) return [];
  const parsed = JSON.parse(raw);
  if (!Array.isArray(parsed)) throw new Error("bad legacy shape");
  return parsed;
}

// Export: legacy entries first (preserved), then all per-email blobs,
// deduped by normalized email. Any storage failure throws -> 503.
async function exportWaitlist(store) {
  const seen = new Set();
  const out = [];
  const add = (rec) => {
    if (!rec || typeof rec.email !== "string") throw new Error("bad record shape");
    const email = normalize(rec.email);
    if (!email || seen.has(email)) return;
    seen.add(email);
    out.push({ email, ts: rec.ts });
  };
  for (const e of await readLegacy(store)) add(e);
  for await (const page of store.list({ paginate: true })) {
    for (const blob of page.blobs || []) {
      if (blob.key === LEGACY_KEY) continue;
      add(JSON.parse(await store.get(blob.key)));
    }
  }
  return out;
}

export const createWaitlistHandler = (getStoreFactory) => async (req) => {
  const url = new URL(req.url);

  if (req.method === "GET" && url.pathname.endsWith("/health")) {
    return json(200, { ok: true, service: "aegis-waitlist" });
  }

  if (req.method === "GET" && url.pathname.endsWith("/list")) {
    const key = url.searchParams.get("key") || "";
    if (!process.env.ADMIN_KEY || key !== process.env.ADMIN_KEY) {
      return json(403, { ok: false, error: "Forbidden" });
    }
    try {
      const entries = await exportWaitlist(await getStoreFactory());
      return json(200, { ok: true, count: entries.length, signups: entries });
    } catch {
      return json(503, { ok: false, error: "Store unavailable." });
    }
  }

  if (req.method !== "POST") {
    return json(405, { ok: false, error: "Method not allowed" });
  }

  let data;
  try { data = await req.json(); } catch {
    return json(400, { ok: false, error: "Invalid JSON." });
  }
  if (data === null || typeof data !== "object" || Array.isArray(data)) {
    return json(400, { ok: false, error: "Invalid payload." });
  }
  if (String(data.website || "").trim()) {
    return json(200, { ok: true });          // honeypot — accept, store nothing
  }

  const email = normalize(data.email);
  if (!EMAIL_RE.test(email) || email.length > 254) {
    return json(400, { ok: false, error: "Enter a valid email address." });
  }

  let store;
  try { store = await getStoreFactory(); } catch {
    return json(503, { ok: false, error: "Store unavailable." });
  }

  // Read-only legacy duplicate check + per-email key read. Any read
  // failure returns 503 with zero writes.
  try {
    const legacy = await readLegacy(store);
    if (legacy.some((e) => e && normalize(e.email) === email)) {
      return json(200, { ok: true, duplicate: true });
    }
  } catch {
    return json(503, { ok: false, error: "Store unavailable." });
  }

  let existing;
  try { existing = await store.get(keyFor(email)); } catch {
    return json(503, { ok: false, error: "Store unavailable." });
  }
  if (existing) {
    return json(200, { ok: true, duplicate: true });
  }

  // One key per email: concurrent different emails never overwrite each
  // other; same-email races are last-write on that person's own key only.
  try {
    await store.setJSON(keyFor(email), { email, ts: Date.now() });
  } catch {
    return json(503, { ok: false, error: "Store unavailable." });
  }
  return json(200, { ok: true });
};

export default createWaitlistHandler(() => getStoreSafe());

export const config = { path: ["/api/waitlist", "/api/health", "/api/waitlist/list"] };
