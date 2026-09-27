import type { VerificationRun } from "../../app/api";

export function VerificationPanel({ run, running, onVerify }: { run?: VerificationRun; running: boolean; onVerify: () => void }) {
  return <div className="verification"><div className={`verification-status ${running ? "running" : run?.exitCode === 0 ? "passed" : run ? "failed" : ""}`}><span>{running ? "…" : run ? (run.exitCode === 0 ? "✓" : "×") : "—"}</span><div><strong>{running ? "Verification running" : run ? (run.exitCode === 0 ? "Verification passed" : "Verification failed") : "Not run"}</strong><small>{run?.command.join(" ") ?? "ctest --test-dir build"}</small></div></div><button onClick={onVerify} disabled={running}>{running ? "Running…" : "Run check"}</button>{run && <pre>{run.output}</pre>}</div>;
}
