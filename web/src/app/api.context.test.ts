import { describe, expect, it, vi } from "vitest";
import { api, type ApiError, type TaskContext } from "./api";

const context: TaskContext = { goal: "g", constraints: "c", decisions: "d", attempts: "a", unfinished: "u", next_action: "n" };

it("updateTaskContext sends six fields directly and normalizes the task", async () => {
  const fetchMock = vi.fn().mockResolvedValue({
    ok: true,
    json: () => Promise.resolve({ id: "t1", prompt: "p", repository: "/r", status: "active", created_at: 5, context }),
  });
  vi.stubGlobal("fetch", fetchMock);
  const result = await api.updateTaskContext("t1", context);
  const [path, init] = fetchMock.mock.calls[0];
  expect(path).toBe("/api/tasks/t1/context");
  expect(init.method).toBe("PATCH");
  const body = JSON.parse(init.body);
  expect(body).toEqual(context);
  expect(body).not.toHaveProperty("context");
  expect(result).toEqual(expect.objectContaining({ id: "t1", prompt: "p", repository: "/r", status: "active", createdAt: 5, context }));
  vi.unstubAllGlobals();
});

it("updateTaskContext accepts an expected revision and normalizes context_revision", async () => {
  const fetchMock = vi.fn().mockResolvedValue({
    ok: true,
    json: () => Promise.resolve({ id: "t1", prompt: "p", repository: "/r", status: "active", created_at: 5, context, context_revision: 7 }),
  });
  vi.stubGlobal("fetch", fetchMock);
  const result = await api.updateTaskContext("t1", context, 6);
  const [, init] = fetchMock.mock.calls[0];
  const body = JSON.parse(init.body);
  expect(body.expected_revision).toBe(6);
  expect(result).toEqual(expect.objectContaining({ id: "t1", context, context_revision: 7 }));
  vi.unstubAllGlobals();
});

it("attach status and normalized currentTask on 409 conflict errors", async () => {
  const rawTask = { id: "t1", prompt: "p", repository: "/r", status: "active", created_at: 5, context, context_revision: 7 };
  const fetchMock = vi.fn().mockResolvedValue({
    ok: false,
    status: 409,
    json: () => Promise.resolve({ error: { code: "context_conflict", message: "stale revision" }, current_task: rawTask }),
  });
  vi.stubGlobal("fetch", fetchMock);
  const error = await api.updateTaskContext("t1", context, 6).then(() => { throw new Error("Expected rejection"); }, (e: ApiError) => e);
  expect(error.status).toBe(409);
  expect(error.message).toBe("stale revision");
  expect(error.currentTask).toEqual(expect.objectContaining({ id: "t1", context, context_revision: 7 }));
  vi.unstubAllGlobals();
});
