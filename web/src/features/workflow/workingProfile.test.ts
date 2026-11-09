import { describe, expect, it, beforeEach } from "vitest";
import { loadProfile, saveProfile, validateProfile, formatWorkflow } from "./workingProfile";

const repo = "test/repo";
const key = "aegis:profile:" + repo;
const profile = {
  version: 1 as const,
  instructions: [{ text: "be terse", source: "user" }],
  workflows: [{ name: "w", steps: [{ purpose: "p", context: "c", agent: "a", result: "r", evidenceNeeded: "e" }] }],
};

beforeEach(() => localStorage.clear());

describe("save/load roundtrip", () => {
  it("save then load preserves source and roundtrips", () => {
    saveProfile(repo, profile);
    const loaded = loadProfile(repo);
    expect(loaded).toEqual(profile);
    expect(loaded?.instructions[0].source).toBe("user");
  });

  it("malformed saved JSON is preserved and load throws", () => {
    localStorage.setItem(key, "{not json");
    expect(() => loadProfile(repo)).toThrow(/malformed/);
    expect(localStorage.getItem(key)).toBe("{not json");
  });

  it("rejects more than 20 instructions", () => {
    const big = { ...profile, instructions: Array.from({ length: 21 }, (_, i) => ({ text: `t${i}`, source: "s" })) };
    expect(() => validateProfile(big)).toThrow(/20/);
  });

  it("formatWorkflow includes step evidence", () => {
    const text = formatWorkflow(profile.workflows[0]);
    expect(text).toContain("Workflow: w");
    expect(text).toContain("purpose: p");
    expect(text).toContain("evidence needed: e");
  });
});
