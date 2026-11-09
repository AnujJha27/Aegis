import { useEffect, useState } from "react";
import type { VerificationRun } from "../../app/api";

const DEFAULT_COMMAND = "ctest\n--test-dir\nbuild\n--no-tests=error";
const storageKey = (repository?: string) => `aegis:verification-command:${repository ?? "default"}`;

function parseCommand(text: string): string[] | null {
  const source = text.endsWith("\n") ? text.slice(0, -1) : text;
  if (!source.length) return null;
  const args = source.split("\n");
  if (!args[0].trim()) return null;
  if (args.some((argument) => argument.includes("\0"))) return null;
  return args;
}

export function VerificationPanel({ run, running, onVerify, repository }: { run?: VerificationRun; running: boolean; onVerify: (command?: string[]) => void; repository?: string }) {
  const [commandText, setCommandText] = useState(DEFAULT_COMMAND);
  const [error, setError] = useState("");
  useEffect(() => {
    let stored: string | null = null;
    try { stored = window.localStorage.getItem(storageKey(repository)); } catch { /* storage unavailable */ }
    setCommandText(stored ?? DEFAULT_COMMAND);
    setError("");
  }, [repository]);
  function updateCommand(text: string) {
    setCommandText(text);
    try { window.localStorage.setItem(storageKey(repository), text); } catch { /* storage unavailable */ }
  }
  function submit() {
    const command = parseCommand(commandText);
    if (!command) { setError("Enter a verification command, one argument per line; the first line must be an executable."); return; }
    setError("");
    onVerify(command);
  }
  const freshness = run?.freshness ?? "unknown";
  const passed = Boolean(run && run.exitCode === 0 && freshness === "current");
  const failed = Boolean(run && run.exitCode !== 0);
  const heading = running ? "Verification running" : !run ? "Not run" : passed ? "Verification passed" : failed ? "Verification failed" : freshness === "stale" ? "Verification stale" : "Verification freshness unknown";
  return <div className="verification"><div className={`verification-status ${running ? "running" : passed ? "passed" : failed ? "failed" : run ? "" : ""}`}><span>{running ? "…" : run ? (passed ? "✓" : "×") : "—"}</span><div><strong>{heading}</strong>{failed && freshness !== "current" && <small>Freshness: {freshness} (historical result)</small>}<small>{run?.command.join(" ") ?? "ctest --test-dir build"}</small></div></div><label htmlFor="verification-command">Verification command</label><textarea id="verification-command" value={commandText} disabled={running} rows={4} spellCheck={false} onChange={(event) => updateCommand(event.target.value)} /><small>one argument per line</small>{error && <p role="alert">{error}</p>}<button onClick={submit} disabled={running}>{running ? "Running…" : "Run check"}</button>{run?.output && <details className="verification-output"><summary>Show output</summary><pre>{run.output}</pre></details>}</div>;
}
