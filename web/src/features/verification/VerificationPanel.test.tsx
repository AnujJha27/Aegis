import { cleanup, fireEvent, render, screen } from "@testing-library/react";
import { afterEach, describe, expect, it } from "vitest";
import { VerificationPanel } from "./VerificationPanel";

afterEach(cleanup);

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
