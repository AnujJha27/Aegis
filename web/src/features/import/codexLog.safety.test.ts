import { describe, expect, it } from "vitest";
import { parseCodexLog } from "./codexLog";

const utf8 = (s: string) => new TextEncoder().encode(s).length;

describe("codexLog safety", () => {
  it("excludes assistant phase-analysis messages", () => {
    const lines = [
      JSON.stringify({ type: "session_meta", payload: { id: "s1" } }),
      JSON.stringify({ type: "response_item", payload: { id: "a1", type: "message", role: "assistant", phase: "analysis", content: [{ type: "output_text", text: "reviewing the repo structure next." }] } }),
      JSON.stringify({ type: "response_item", payload: { id: "a2", type: "message", role: "assistant", content: [{ type: "output_text", text: "Here is the summary." }] } }),
    ];
    const log = parseCodexLog(lines.join("\n"), "t.jsonl");
    expect(log.messages.map((m) => m.text)).toEqual(["Here is the summary."]);
  });

  it("bounds total message bytes at 500000 with 60x12000 visible messages", () => {
    const lines = [JSON.stringify({ type: "session_meta", payload: { id: "s2" } })];
    for (let i = 0; i < 60; i++) {
      lines.push(JSON.stringify({ type: "response_item", payload: { id: `u${i}`, type: "message", role: "user", content: [{ type: "input_text", text: "x".repeat(12000) }] } }));
    }
    const log = parseCodexLog(lines.join("\n"), "t.jsonl");
    const totalBytes = log.messages.reduce((n, m) => n + utf8(m.text), 0);
    expect(totalBytes).toBeLessThanOrEqual(500000);
    expect(log.messages.length).toBeLessThan(60);
  });
});