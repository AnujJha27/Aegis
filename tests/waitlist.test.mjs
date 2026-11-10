import { test } from "node:test";
import assert from "node:assert/strict";
import { createWaitlistHandler } from "../netlify/functions/waitlist.mjs";

process.env.ADMIN_KEY = "test-key";

const EMAILS = (store) =>
  [...store.data.keys()].sort().map((k) => store.data.get(k).email);

class MockStore {
  constructor({ legacy = null, failRead = false } = {}) {
    this.data = new Map(); // key -> record object
    this.legacy = legacy;
    this.failRead = failRead;
    this.writes = 0;
  }
  async get(key) {
    if (this.failRead) throw new Error("read error");
    if (key === "signups") return this.legacy;
    return this.data.has(key) ? JSON.stringify(this.data.get(key)) : null;
  }
  async setJSON(key, val) {
    this.writes++;
    this.data.set(key, val);
  }
  async *list({ paginate } = {}) {
    const keys = [...this.data.keys()];
    if (paginate) {
      for (let i = 0; i < keys.length; i += 2) {
        yield { blobs: keys.slice(i, i + 2).map((key) => ({ key })) };
      }
    } else {
      yield { blobs: keys.map((key) => ({ key })) };
    }
  }
}

const makeHandler = (opts) => {
  const store = new MockStore(opts);
  return { store, handler: createWaitlistHandler(() => store) };
};

const post = (handler, body) =>
  handler(
    new Request("https://site/api/waitlist", {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: typeof body === "string" ? body : JSON.stringify(body),
    })
  );

const get = (handler, path) => handler(new Request("https://site" + path));

test("valid email is accepted with no position field", async () => {
  const { handler } = makeHandler();
  const res = await post(handler, { email: "a@b.co" });
  const j = await res.json();
  assert.equal(res.status, 200);
  assert.equal(j.ok, true);
  assert.equal("position" in j, false);
});

test("concurrent different emails both persist", async () => {
  const { handler, store } = makeHandler();
  const [r1, r2] = await Promise.all([
    post(handler, { email: "one@x.co" }),
    post(handler, { email: "two@x.co" }),
  ]);
  assert.equal((await r1.json()).ok, true);
  assert.equal((await r2.json()).ok, true);
  assert.deepEqual(EMAILS(store).sort(), ["one@x.co", "two@x.co"]);
});

test("read error returns 503 with zero writes", async () => {
  const { handler, store } = makeHandler({ failRead: true });
  const res = await post(handler, { email: "a@b.co" });
  assert.equal(res.status, 503);
  assert.equal(store.writes, 0);
});

test("malformed legacy blob returns 503 with zero writes", async () => {
  for (const bad of ["{not json", '{"a":1}']) {
    const { handler, store } = makeHandler({ legacy: bad });
    const res = await post(handler, { email: "a@b.co" });
    assert.equal(res.status, 503);
    assert.equal(store.writes, 0);
  }
});

test("null input returns 400", async () => {
  const { handler } = makeHandler();
  const res = await post(handler, "null");
  assert.equal(res.status, 400);
});

test("paginated admin export includes legacy + new, deduped", async () => {
  const legacy = JSON.stringify([
    { email: "Old@X.co", ts: 111 },
    { email: "legacy2@x.co", ts: 222 },
  ]);
  const { handler, store } = makeHandler({ legacy });
  store.data.set("signup:new1@x.co", { email: "new1@x.co", ts: 333 });
  store.data.set("signup:new2@x.co", { email: "new2@x.co", ts: 444 });
  store.data.set("signup:old@x.co", { email: "old@x.co", ts: 555 }); // dup of legacy

  const res = await get(handler, "/api/waitlist/list?key=test-key");
  const j = await res.json();
  assert.equal(res.status, 200);
  assert.equal(j.ok, true);
  assert.deepEqual(
    j.signups.map((s) => s.email).sort(),
    ["legacy2@x.co", "new1@x.co", "new2@x.co", "old@x.co"]
  );
  assert.equal(j.signups.find((s) => s.email === "old@x.co").ts, 111); // legacy preserved
  assert.equal(j.count, 4);
});

test("duplicate against legacy blob returns ok duplicate, no write", async () => {
  const legacy = JSON.stringify([{ email: "old@x.co", ts: 1 }]);
  const { handler, store } = makeHandler({ legacy });
  const res = await post(handler, { email: "OLD@x.co" });
  const j = await res.json();
  assert.equal(res.status, 200);
  assert.equal(j.ok, true);
  assert.equal(j.duplicate, true);
  assert.equal(store.writes, 0);
});