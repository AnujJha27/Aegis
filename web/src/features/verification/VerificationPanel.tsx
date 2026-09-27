import type { VerificationRun } from "../../app/api";

export function VerificationPanel({ run, onVerify }: { run?: VerificationRun; onVerify: () => void }) {
  return <div className="verification"><div className={`verification-status ${run?.exitCode === 0 ? "passed" : run ? "failed" : ""}`}><span>{run ? (run.exitCode === 0 ? "✓" : "×") : "—"}</span><div><strong>{run ? (run.exitCode === 0 ? "Verification passed" : "Verification failed") : "Not run"}</strong><small>{run?.command.join(" ") ?? "ctest --test-dir build"}</small></div></div><button onClick={onVerify}>Run check</button>{run && <pre>{run.output}</pre>}</div>;
}
