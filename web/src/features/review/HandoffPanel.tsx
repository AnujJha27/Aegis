import { useState } from "react";
import type { HandoffContext } from "../../app/api";
import { readable } from "../../app/events";
import { formatHandoff } from "./handoffText";

export function HandoffPanel({ context }: { context?: HandoffContext }) {
  const [copied, setCopied] = useState(false);
  if (!context) return <p className="muted">Select a task to build handoff context.</p>;
  const verification = context.verification;
  const verificationFreshness = verification ? verification.freshness ?? "unknown" : null;
  const verificationStatus = verification
    ? verification.exitCode === 0
      ? verificationFreshness === "current" ? "Passed" : verificationFreshness === "stale" ? "Stale (historical check; exit 0)" : "Freshness unknown (historical check; exit 0)"
      : `Failed (exit ${verification.exitCode})`
    : "No verification recorded";
  const text = formatHandoff(context);
  async function copy() { await navigator.clipboard.writeText(text); setCopied(true); window.setTimeout(() => setCopied(false), 1200); }
  return <div className="handoff"><div className="handoff-actions"><span>Bounded context · {context.recent_events.length} recent events</span><button onClick={copy}>{copied ? "Copied" : "Copy context"}</button></div><div className="handoff-grid"><div><div className="subheading">TASK</div><p>{context.prompt}</p><div className="subheading">CHANGED FILES</div>{context.changed_files.length ? <ul>{context.changed_files.map((file) => <li key={file}>{file}</li>)}</ul> : <p className="muted">No changed files.</p>}{context.changed_files_truncated && <p className="muted">Additional paths omitted (100-file handoff limit).</p>}<div className="subheading">LATEST VERIFICATION</div>{verification ? <><p>{verificationStatus}</p>{verification.exitCode !== 0 && verificationFreshness !== "current" && <small>Freshness: {verificationFreshness} (historical result)</small>}<pre>{verification.command.join(" ")}\n{verification.output || "(no output)"}</pre></> : <p className="muted">No verification recorded.</p>}<div className="subheading">REVIEW FINDINGS</div>{context.findings.length ? <ul>{context.findings.map((finding) => <li key={finding.id}><strong>{finding.status}</strong> · {finding.file_path}{finding.start_line ? `:${finding.start_line}${finding.end_line && finding.end_line !== finding.start_line ? `-${finding.end_line}` : ""}` : ""}<br />{finding.message}</li>)}</ul> : <p className="muted">No review findings.</p>}</div><div><div className="subheading">RECENT ACTIVITY</div><pre>{context.recent_events.map((event) => `${event.agent} · ${event.type}\n${readable(event.content).slice(0, 800)}`).join("\n\n") || "No activity yet."}</pre></div></div></div>;
}
