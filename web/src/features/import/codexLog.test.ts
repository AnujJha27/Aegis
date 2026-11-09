import { test, expect } from "vitest";
import { parseCodexLog, evidenceCards } from "./codexLog";

const meta = (id = "sess-1") => JSON.stringify({ type: "session_meta", payload: { id, parent_thread_id: "pt-1", cwd: "/repo" } });

const userMsg = (text: string, id = "i1") => JSON.stringify({ type: "response_item", payload: { type: "message", id, role: "user", content: [{ type: "input_text", text }] } });
const asstMsg = (text: string, id = "i2") => JSON.stringify({ type: "response_item", payload: { type: "message", id, role: "assistant", content: [{ type: "output_text", text }] } });
const fnCall = (text: string, id = "i3", call_id = id) => JSON.stringify({ type: "response_item", payload: { type: "function_call", id, call_id, content: [{ type: "input_text", text }] } });
const fnOut = (text: string, id = "i4", call_id = "i3") => JSON.stringify({ type: "response_item", payload: { type: "function_call_output", id, call_id, output: text } });

const basicLog = [meta(), userMsg("fix the bug"), asstMsg("I will fix it"), fnCall("cargo test"), fnOut("test ok")].join("\n");

test("parses visible messages with provenance", () => {
  const log = parseCodexLog(basicLog, "fake.jsonl");
  expect(log.sessionId).toBe("sess-1");
  expect(log.parentThreadId).toBe("pt-1");
  expect(log.cwd).toBe("/repo");
  expect(log.messages.map((m) => m.role)).toEqual(["user", "assistant", "tool", "tool"]);
  expect(log.messages[0]).toMatchObject({ text: "fix the bug", source_line: 2, source_name: "fake.jsonl" });
  expect(log.messages[3].text).toContain("historical record");
  expect(log.warnings).toEqual([]);
});

test("never leaks reasoning, system, or developer content", () => {
  const lines = [
    meta(),
    JSON.stringify({ type: "response_item", payload: { type: "reasoning", id: "r1", summary: "secret thoughts" } }),
    JSON.stringify({ type: "response_item", payload: { type: "message", id: "r2", role: "system", content: [{ type: "input_text", text: "system prompt" }] } }),
    JSON.stringify({ type: "response_item", payload: { type: "message", id: "r3", role: "developer", content: [{ type: "input_text", text: "developer prompt" }] } }),
    userMsg("visible"),
  ].join("\n");
  const log = parseCodexLog(lines, "log.jsonl");
  expect(log.messages).toHaveLength(1);
  expect(log.messages[0].text).toBe("visible");
  expect(JSON.stringify(log.messages)).not.toContain("secret");
});

test("mirrored event_msg deduped when canonical items present", () => {
  const lines = [meta(), userMsg("canonical user"), JSON.stringify({ type: "event_msg", payload: { type: "user_message", message: "mirror user" } }), JSON.stringify({ type: "event_msg", payload: { type: "agent_message", message: "mirror agent" } })].join("\n");
  const log = parseCodexLog(lines, "log.jsonl");
  expect(log.messages).toHaveLength(1);
  expect(log.messages[0].text).toBe("canonical user");
  expect(log.warnings.some((w) => w.includes("mirrored"))).toBe(true);
});

test("event fallback used only when no canonical messages", () => {
  const lines = [meta(), JSON.stringify({ type: "event_msg", payload: { type: "user_message", message: "fallback user" } })].join("\n");
  const log = parseCodexLog(lines, "log.jsonl");
  expect(log.messages).toHaveLength(1);
  expect(log.messages[0].text).toBe("fallback user");
});

test("event_msg fallback dedupes repeated payload.id, not identical text", () => {
  const ev = (id: string) => JSON.stringify({ type: "event_msg", payload: { type: "user_message", id, message: "same text" } });
  const lines = [meta(), ev("e1"), ev("e1"), ev("e2")].join("\n");
  const log = parseCodexLog(lines, "log.jsonl");
  expect(log.messages).toHaveLength(2);
  expect(log.messages[0].id).not.toBe(log.messages[1].id);
});

test("malformed and non-object lines warn, line numbers preserved", () => {
  const lines = [meta(), "{not json", "[1,2,3]", userMsg("after junk")].join("\n");
  const log = parseCodexLog(lines, "log.jsonl");
  expect(log.messages).toHaveLength(1);
  expect(log.messages[0].source_line).toBe(4);
  expect(log.warnings).toHaveLength(2);
});

test("rejects conflicting session ids", () => {
  const lines = [meta("a"), meta("b")].join("\n");
  expect(() => parseCodexLog(lines, "log.jsonl")).toThrow(/Conflicting session ids/);
});

test("size bounds", () => {
  expect(() => parseCodexLog("x".repeat(8 * 1024 * 1024 + 1), "big.jsonl")).toThrow(/8 MiB/);
  const long = "y".repeat(13000);
  const log = parseCodexLog([meta(), userMsg(long)].join("\n"), "log.jsonl");
  expect(new TextEncoder().encode(log.messages[0].text).length).toBeLessThanOrEqual(12000 + 10);
  expect(log.warnings.some((w) => w.includes("truncated"))).toBe(true);
});

test("missing tool output stays warning, not fabricated", () => {
  const lines = [meta(), userMsg("go"), JSON.stringify({ type: "response_item", payload: { type: "function_call_output", id: "i9" } })].join("\n");
  const log = parseCodexLog(lines, "log.jsonl");
  expect(log.warnings.some((w) => w.includes("missing output"))).toBe(true);
  expect(log.messages[1].text).toContain("missing tool output");
});

test("unmatched tool call warns, never fabricated", () => {
  const lines = [meta(), userMsg("go"), fnCall("cargo build", "c1")].join("\n");
  const log = parseCodexLog(lines, "log.jsonl");
  expect(log.warnings.some((w) => w.includes("no matching tool output"))).toBe(true);
  expect(log.messages.some((m) => m.text.includes("cargo build"))).toBe(true);
  expect(log.messages.every((m) => !m.text.includes("test ok"))).toBe(true);
});

test("private phase analysis in event fallback excluded, final retained", () => {
  const lines = [
    meta(),
    JSON.stringify({ type: "event_msg", payload: { type: "agent_message", message: "internal analysis notes", phase: "analysis" } }),
    JSON.stringify({ type: "event_msg", payload: { type: "agent_message", message: "final answer" } }),
  ].join("\n");
  const log = parseCodexLog(lines, "log.jsonl");
  expect(log.messages).toHaveLength(1);
  expect(log.messages[0].text).toBe("final answer");
  expect(JSON.stringify(log.messages)).not.toContain("internal analysis");
});

test("throws without session or messages", () => {
  expect(() => parseCodexLog(userMsg("x"), "log.jsonl")).toThrow(/session_meta/);
  expect(() => parseCodexLog(meta(), "log.jsonl")).toThrow(/No visible messages/);
});

test("evidenceCards are deterministic, no preferences inferred", () => {
  const log = parseCodexLog(basicLog, "fake.jsonl");
  const cards = evidenceCards(log);
  expect(cards).toEqual(evidenceCards(log));
  expect(cards[0].kind).toBe("task_state");
  expect(cards[2].kind).toBe("tool_fact");
  expect(cards.every((c) => c.source_line > 0)).toBe(true);
});