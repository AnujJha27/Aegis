import { describe, expect, it, vi } from "vitest";
import { api, type TaskContext } from "./api";

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
  expect(result).toEqual({ id: "t1", prompt: "p", repository: "/r", status: "active", createdAt: 5, context });
  vi.unstubAllGlobals();
});
