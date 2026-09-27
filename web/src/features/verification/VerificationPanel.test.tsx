import { cleanup, render, screen } from "@testing-library/react";
import { afterEach, describe, expect, it } from "vitest";
import { VerificationPanel } from "./VerificationPanel";

afterEach(cleanup);

describe("verification status", () => {
  it("shows an in-progress state and disables another run", () => {
    render(<VerificationPanel running run={undefined} onVerify={() => {}} />);
    expect(screen.getByText("Verification running")).toBeTruthy();
    expect(screen.getByRole("button", { name: "Running…" }).hasAttribute("disabled")).toBe(true);
  });
});
