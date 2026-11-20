import type { AgentEvent } from "../../app/api";
import { readable } from "../../app/events";

const LIMIT = 16000;
type Rec = Record<string, unknown>;

function asRec(value: unknown): Rec | null {
  if (typeof value === "string") { try { value = JSON.parse(value); } catch { return null; } }
  return typeof value === "object" && value !== null ? value as Rec : null;
}

function bounded(text: string, label: string): string {
  if (!text.trim()) return `${label}: No result captured`;
  return text.length > LIMIT ? `${text.slice(0, LIMIT)}\n… [${label} omitted: ${text.length - LIMIT} characters]` : text;
}

function text(value: unknown): string {
  if (value == null) return "";
  try { return readable(typeof value === "string" ? value : (JSON.stringify(value, null, 2) ?? "")); } catch { return String(value); }
}

type Tool = { runId: string; id: string; name: string; input: string; result: string; failed: boolean; completed: boolean };

export function ToolActivity({ events, runFinished }: { events: AgentEvent[]; runFinished: boolean }) {
  const tools = new Map<string, Tool>();
  for (const event of events) {
    let rec = asRec(event.content);
    const legacy = event.type === "command.started" || event.type === "command.completed";
    if (!rec) {
      if (legacy && typeof event.content === "string" && event.content.trim()) {
        rec = { name: "Command", details: event.content };
      } else {
        continue;
      }
    }
    if (!legacy && !event.type.startsWith("tool.")) continue;
    const keyId = text(rec.tool_use_id ?? rec.id) || event.id;
    const key = `${event.runId}::${keyId}`;
    const tool = tools.get(key) ?? { runId: event.runId, id: keyId, name: text(rec.name), input: "", result: "", failed: false, completed: false };
    if (event.type === "tool.started" || event.type === "command.started") {
      if (!tool.name) tool.name = text(rec.name);
      const input = rec.input ?? asRec(rec.details) ?? rec;
      tool.input = text(input);
    } else if (event.type === "tool.completed" || event.type === "command.completed") {
      tool.completed = true;
      if (rec.is_error === true || rec.status === "failed") tool.failed = true;
      const details = asRec(rec.details);
      if (details && !tool.name) tool.name = text(details.name);
      tool.result = text(rec.content ?? rec.details ?? rec.result);
      if (!tool.input && details) tool.input = text(details.input ?? details);
    } else if (event.type === "tool.updated") {
      if (rec.status === "failed") tool.failed = true;
      if (!tool.name) tool.name = text(rec.name);
      if (rec.details != null || rec.content != null) tool.result = text(rec.content ?? rec.details);
    }
    tools.set(key, tool);
  }
  if (!tools.size) return null;
  return <>{[...tools.values()].map((tool) => {
    const detail = asRec(tool.result) ?? asRec(tool.input);
    const hint = text(detail?.command ?? detail?.path ?? detail?.query ?? detail?.file_path ?? detail?.prompt);
    const status = tool.failed ? "Failed" : tool.completed ? "Complete" : runFinished ? "No recorded result" : "In progress";
    return <details key={`${tool.runId}::${tool.id}`} className="tool-activity">
      <summary><span className={`tool-status ${tool.failed ? "failed" : tool.completed ? "done" : "running"}`}>{status}</span> {tool.name || "tool"}{hint ? ` · ${hint.slice(0, 120)}` : ""} <code>{tool.id}</code></summary>
      <pre>{bounded(tool.input, "Input")}</pre>
      <pre>{tool.completed || tool.result ? bounded(tool.result, "Result") : "No result captured"}</pre>
    </details>;
  })}</>;
}
