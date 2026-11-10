// Aegis waitlist — Netlify function (Web-standard, v2 runtime).
// Storage: Netlify Blobs (auto-provisioned, persists across deploys).
// Signups: GET /api/waitlist/list?key=ADMIN_KEY  (set ADMIN_KEY env to enable)

import { getStore } from "@netlify/blobs";

const EMAIL_RE = /^[^@\s]+@[^@\s]+\.[^@\s]+$/;
const json = (status, obj) =>
  new Response(JSON.stringify(obj), {
    status,
    headers: { "content-type": "application/json" },
  });

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
    const store = getStore({ name: "waitlist", consistency: "strong" });
    const list = await store.list();
    const out = [];
    for (const item of list.items) {
      const raw = await store.get(item.key);
      try {
        const parsed = JSON.parse(raw);
        if (parsed && typeof parsed === "object" && !Array.isArray(parsed)) {
          out.push(parsed);
        } else {
          out.push({ email: item.key });
        }
      } catch { out.push({ email: item.key }); }
    }
    return json(200, { ok: true, count: out.length, signups: out });
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
    const store = getStore({ name: "waitlist", consistency: "strong" });
    const existing = await store.get(email);
    if (existing) return json(200, { ok: true, duplicate: true });
    const list = await store.list();
    const position = (list.items?.length || 0) + 1;
    await store.setJSON(email, { email, ts: Date.now() });
    return json(200, { ok: true, position });
  } catch (err) {
    return json(500, { ok: false, error: "Store unavailable." });
  }
};

export const config = { path: ["/api/waitlist", "/api/health", "/api/waitlist/list"] };
