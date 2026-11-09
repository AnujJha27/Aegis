import { cleanup, fireEvent, render, screen } from "@testing-library/react";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { VerificationPanel } from "./VerificationPanel";

afterEach(() => { cleanup(); vi.restoreAllMocks(); });

describe("verification status", () => {
  it("shows an in-progress state and disables another run", () => {
    render(<VerificationPanel running run={undefined} onVerify={() => {}} />);
    expect(screen.getByText("Verification running")).toBeTruthy();
    expect(screen.getByRole("button", { name: "Running…" }).hasAttribute("disabled")).toBe(true);
  });

  it("keeps completed output collapsed until requested", () => {
    render(<VerificationPanel running={false} run={{ id: "verify-1", taskId: "task-1", runId: null, command: ["ctest"], exitCode: 1, output: "test failed", startedAt: 1, finishedAt: 2 }} onVerify={() => {}} />);
    const details = screen.getByText("Show output").closest("details");
    expect(details?.hasAttribute("open")).toBe(false);

    fireEvent.click(screen.getByText("Show output"));
    expect(details?.hasAttribute("open")).toBe(true);
    expect(screen.getByText("test failed")).toBeTruthy();
  });
});

describe("verification command", () => {
  beforeEach(() => { localStorage.clear(); });

  function mountPanel(props: Record<string, unknown>) {
    return render(<VerificationPanel {...(props as any)} />);
  }

  function commandValue() {
    return (screen.getByLabelText("Verification command") as HTMLTextAreaElement).value;
  }

  it("shows a labelled textarea with the safe default command", () => {
    mountPanel({ running: false, onVerify: () => {}, repository: "/repo/a" });
    expect(screen.getByLabelText("Verification command")).toBeTruthy();
    expect(commandValue()).toBe("ctest\n--test-dir\nbuild\n--no-tests=error");
    expect(screen.getByText("one argument per line")).toBeTruthy();
  });

  it("runs the edited command as one argument per line", () => {
    const onVerify = vi.fn();
    mountPanel({ running: false, onVerify, repository: "/repo/a" });
    fireEvent.change(screen.getByLabelText("Verification command"), { target: { value: "npm\ntest" } });
    fireEvent.click(screen.getByRole("button", { name: "Run check" }));
    expect(onVerify).toHaveBeenCalledWith(["npm", "test"]);
  });

  it("blocks running with a blank command and shows validation", () => {
    const onVerify = vi.fn();
    mountPanel({ running: false, onVerify, repository: "/repo/a" });
    fireEvent.change(screen.getByLabelText("Verification command"), { target: { value: "   \n  " } });
    fireEvent.click(screen.getByRole("button", { name: "Run check" }));
    expect(onVerify).not.toHaveBeenCalled();
    expect(screen.getByRole("alert")).toBeTruthy();
  });

  it("retains the command per repository and isolates other repositories", () => {
    const onVerify = vi.fn();
    const first = mountPanel({ running: false, onVerify, repository: "/repo/a" });
    fireEvent.change(screen.getByLabelText("Verification command"), { target: { value: "npm\ntest" } });
    first.unmount();
    mountPanel({ running: false, onVerify, repository: "/repo/a" });
    expect(commandValue()).toBe("npm\ntest");
    cleanup();
    mountPanel({ running: false, onVerify, repository: "/repo/b" });
    expect(commandValue()).toBe("ctest\n--test-dir\nbuild\n--no-tests=error");
  });

  it("does not let storage errors block a check", () => {
    vi.spyOn(Storage.prototype, "getItem").mockImplementation(() => { throw new Error("storage broken"); });
    vi.spyOn(Storage.prototype, "setItem").mockImplementation(() => { throw new Error("storage broken"); });
    const onVerify = vi.fn();
    mountPanel({ running: false, onVerify, repository: "/repo/a" });
    fireEvent.change(screen.getByLabelText("Verification command"), { target: { value: "npm\ntest" } });
    fireEvent.click(screen.getByRole("button", { name: "Run check" }));
    expect(onVerify).toHaveBeenCalledWith(["npm", "test"]);
  });
});

describe("verification freshness", () => {
  const baseRun = { id: "verify-1", taskId: "task-1", runId: null, command: ["ctest"], exitCode: 0, output: "ok", startedAt: 1, finishedAt: 2 };

  it("does not show passed for a stale run", () => {
    render(<VerificationPanel running={false} run={{ ...baseRun, freshness: "stale" } as any} onVerify={() => {}} />);
    expect(screen.queryByText("Verification passed")).toBeNull();
    expect(screen.getByText("Verification stale")).toBeTruthy();
  });

  it("shows unknown freshness for an unknown run", () => {
    render(<VerificationPanel running={false} run={{ ...baseRun, freshness: "unknown" } as any} onVerify={() => {}} />);
    expect(screen.queryByText("Verification passed")).toBeNull();
    expect(screen.getByText("Verification freshness unknown")).toBeTruthy();
  });

  it("treats a missing freshness field as unknown", () => {
    render(<VerificationPanel running={false} run={baseRun as any} onVerify={() => {}} />);
    expect(screen.queryByText("Verification passed")).toBeNull();
    expect(screen.getByText("Verification freshness unknown")).toBeTruthy();
  });
});
