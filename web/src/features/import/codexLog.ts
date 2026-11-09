export type CodexRole = "user" | "assistant" | "tool";

export type CodexMessage = {
  id: string;
  role: CodexRole;
  text: string;
  source_line: number;
  source_name: string;
};

export type CodexLog = {
  sessionId: string;
  parentThreadId?: string;
  cwd?: string;
  messages: CodexMessage[];
  warnings: string[];
};

const MAX_INPUT_BYTES = 8 * 1024 * 1024;
const MAX_MESSAGES = 1000;
const MAX_MESSAGE_BYTES = 12000;
const MAX_TEXT_BYTES = 500000;

const utf8Bytes = (s: string): number => new TextEncoder().encode(s).length;

const TRUNCATE_MARKER = "…[truncated]";
const truncateUtf8 = (s: string, maxBytes: number): string => {
  const bytes = new TextEncoder().encode(s);
  if (bytes.length <= maxBytes) return s;
  const markerBytes = utf8Bytes(TRUNCATE_MARKER);
  if (maxBytes < markerBytes) return "";
  let out = "";
  let used = 0;
  for (const ch of s) {
    const b = utf8Bytes(ch);
    if (used + b > maxBytes - markerBytes) break;
    out += ch;
    used += b;
  }
  return out + TRUNCATE_MARKER;
};

const textFromContent = (content: unknown): string | null => {
  if (typeof content === "string") return content;
  if (!Array.isArray(content)) return null;
  const parts: string[] = [];
  for (const item of content) {
    if (typeof item !== "object" || item === null) continue;
    const rec = item as Record<string, unknown>;
    if (rec.type !== "input_text" && rec.type !== "output_text") continue;
    if (typeof rec.text === "string") parts.push(rec.text);
  }
  return parts.length ? parts.join("\n") : null;
};

export const parseCodexLog = (text: string, sourceName: string): CodexLog => {
  const warnings: string[] = [];
  if (utf8Bytes(text) > MAX_INPUT_BYTES) {
    throw new Error("Codex log exceeds 8 MiB input limit");
  }
  const lines = text.split("\n");
  let sessionId: string | null = null;
  let parentThreadId: string | undefined;
  let cwd: string | undefined;
  const messages: CodexMessage[] = [];
  const seenItemIds = new Set<string>();
  const seenEventIds = new Set<string>();
  const pendingCalls = new Set<string>();
  let canonicalCount = 0;
  const eventFallback: CodexMessage[] = [];

  lines.forEach((line, idx) => {
    const lineNo = idx + 1;
    const trimmed = line.trim();
    if (!trimmed) return;
    let rec: unknown;
    try {
      rec = JSON.parse(trimmed);
    } catch {
      warnings.push(`Line ${lineNo}: malformed JSON skipped`);
      return;
    }
    if (typeof rec !== "object" || rec === null || Array.isArray(rec)) {
      warnings.push(`Line ${lineNo}: JSON record is not an object, skipped`);
      return;
    }
    const r = rec as Record<string, unknown>;
    const type = typeof r.type === "string" ? r.type : "";
    const payload =
      typeof r.payload === "object" && r.payload !== null
        ? (r.payload as Record<string, unknown>)
        : undefined;
    const recordId = typeof payload?.id === "string" ? payload.id : typeof r.id === "string" ? r.id : `line-${lineNo}`;

    if (type === "session_meta" && payload) {
      const id = typeof payload.id === "string" ? payload.id : typeof payload.session_id === "string" ? payload.session_id : null;
      if (!id) {
        warnings.push(`Line ${lineNo}: session_meta missing id`);
      } else if (sessionId === null) {
        sessionId = id;
      } else if (sessionId !== id) {
        throw new Error(`Conflicting session ids in log (line ${lineNo}): ${sessionId} vs ${id}`);
      }
      if (typeof payload.parent_thread_id === "string") parentThreadId = payload.parent_thread_id;
      if (typeof payload.cwd === "string") cwd = payload.cwd;
      return;
    }

    if (type === "response_item" && payload) {
      if (typeof payload.id === "string") {
        if (seenItemIds.has(payload.id)) {
          warnings.push(`Line ${lineNo}: duplicate response item id ${payload.id} skipped`);
          return;
        }
        seenItemIds.add(payload.id);
      }
      const pType = typeof payload.type === "string" ? payload.type : "";
      const role = payload.role;
      const text = textFromContent(payload.content);
      if (pType === "message" && text !== null) {
        if (role === "user") {
          canonicalCount++;
          messages.push({ id: `sess:PENDING:item:${recordId}`, role: "user", text: truncateUtf8(text, MAX_MESSAGE_BYTES), source_line: lineNo, source_name: sourceName });
          if (utf8Bytes(text) > MAX_MESSAGE_BYTES) warnings.push(`Line ${lineNo}: message truncated to 12000 UTF-8 bytes`);
        } else if (role === "assistant" && (payload.phase === "analysis" || payload.channel === "analysis")) {
          // assistant phase-analysis notes are internal, not visible dialogue
        } else if (role === "assistant") {
          canonicalCount++;
          messages.push({ id: `sess:PENDING:item:${recordId}`, role: "assistant", text: truncateUtf8(text, MAX_MESSAGE_BYTES), source_line: lineNo, source_name: sourceName });
          if (utf8Bytes(text) > MAX_MESSAGE_BYTES) warnings.push(`Line ${lineNo}: message truncated to 12000 UTF-8 bytes`);
        } else {
          // system/developer or non-visible roles: never surfaced
        }
      } else if (pType === "function_call" || pType === "custom_tool_call") {
        const callId = typeof payload.call_id === "string" ? payload.call_id : null;
        if (callId) pendingCalls.add(callId);
        const command = pType === "custom_tool_call" && typeof payload.input === "string" ? payload.input : textFromContent(payload.content) ?? (typeof payload.command === "string" ? payload.command : typeof payload.arguments === "string" ? payload.arguments : null);
        messages.push({ id: `sess:PENDING:item:${recordId}`, role: "tool", text: truncateUtf8(`[historical record — not executed here] ${command ?? "(function call with no readable arguments)"}`, MAX_MESSAGE_BYTES), source_line: lineNo, source_name: sourceName });
      } else if (pType === "function_call_output" || pType === "custom_tool_call_output") {
        const callId = typeof payload.call_id === "string" ? payload.call_id : null;
        if (callId) pendingCalls.delete(callId);
        const out = textFromContent(payload.output) ?? textFromContent(payload.content) ?? null;
        if (out === null) {
          warnings.push(`Line ${lineNo}: function_call_output missing output text (left as missing)`);
          messages.push({ id: `sess:PENDING:item:${recordId}`, role: "tool", text: "(missing tool output)", source_line: lineNo, source_name: sourceName });
        } else {
          messages.push({ id: `sess:PENDING:item:${recordId}`, role: "tool", text: truncateUtf8(`[historical record — not executed here] ${out}`, MAX_MESSAGE_BYTES), source_line: lineNo, source_name: sourceName });
        }
      }
      // reasoning and other response types skipped entirely
      return;
    }

    if (type === "event_msg" && payload) {
      const eType = typeof payload.type === "string" ? payload.type : "";
      const text = textFromContent(payload.content) ?? (typeof payload.message === "string" ? payload.message : null);
      if ((eType === "user_message" || eType === "agent_message") && text !== null) {
        if (eType === "agent_message" && (payload.phase === "analysis" || payload.channel === "analysis")) {
          // internal analysis mirrored via events is not visible dialogue
        } else if (typeof payload.id === "string") {
          if (seenEventIds.has(payload.id)) {
            warnings.push(`Line ${lineNo}: duplicate event_msg id ${payload.id} skipped`);
          } else {
            seenEventIds.add(payload.id);
            eventFallback.push({ id: `sess:PENDING:event:${recordId}`, role: eType === "user_message" ? "user" : "assistant", text, source_line: lineNo, source_name: sourceName });
          }
        } else {
          eventFallback.push({ id: `sess:PENDING:event:${recordId}`, role: eType === "user_message" ? "user" : "assistant", text, source_line: lineNo, source_name: sourceName });
        }
      }
      // all other event types skipped
      return;
    }
    // world_state, token_usage, turn_context, unknown: skipped
  });

  for (const callId of pendingCalls) {
    warnings.push(`Tool call ${callId} has no matching tool output (missing tool output)`);
  }
  if (warnings.length > 100) {
    warnings.length = 100;
    warnings.push("…[additional warnings suppressed]");
  }

  if (canonicalCount > 0 && eventFallback.length > 0) {
    warnings.push(`Skipped ${eventFallback.length} mirrored event_msg entries; canonical response_item messages present`);
  } else if (canonicalCount === 0 && eventFallback.length > 0) {
    messages.push(...eventFallback.slice(0, MAX_MESSAGES).map((m) => ({ ...m, text: truncateUtf8(m.text, MAX_MESSAGE_BYTES) })));
    if (eventFallback.length > MAX_MESSAGES) warnings.push(`Message limit reached; ${eventFallback.length - MAX_MESSAGES} messages dropped`);
  }

  if (messages.length > MAX_MESSAGES) {
    warnings.push(`Message limit reached; ${messages.length - MAX_MESSAGES} messages dropped`);
    messages.length = MAX_MESSAGES;
  }
  const prefix = `sess:${sessionId ?? "unknown"}`;
  for (const m of messages) m.id = m.id.split("sess:PENDING:").join(`${prefix}:`);
  for (const m of eventFallback) m.id = m.id.split("sess:PENDING:").join(`${prefix}:`);

  const total: string[] = [];
  let totalBytes = 0;
  for (let i = 0; i < messages.length; i++) {
    const m = messages[i];
    const b = utf8Bytes(m.text);
    if (totalBytes + b > MAX_TEXT_BYTES) {
      const truncated = truncateUtf8(m.text, MAX_TEXT_BYTES - totalBytes);
      warnings.push(`Line ${m.source_line}: truncated to fit overall 500000 byte bound`);
      if (truncated) {
        m.text = truncated;
        totalBytes = MAX_TEXT_BYTES;
        messages.length = i + 1;
      } else {
        messages.length = i;
      }
      break;
    }
    totalBytes += b;
    total.push(m.text);
  }

  if (!sessionId) throw new Error("No session_meta with a session id found in log");
  if (messages.length === 0) throw new Error("No visible messages recognized in Codex log");

  return { sessionId, parentThreadId, cwd, messages, warnings };
};

export type EvidenceCard = {
  kind: "tool_fact" | "task_state";
  role: CodexRole;
  text: string;
  source_line: number;
  source_name: string;
};

export const evidenceCards = (log: CodexLog): EvidenceCard[] => {
  const cards: EvidenceCard[] = [];
  for (const m of log.messages) {
    if (m.role === "tool") {
      cards.push({ kind: "tool_fact", role: m.role, text: m.text, source_line: m.source_line, source_name: m.source_name });
    } else {
      cards.push({ kind: "task_state", role: m.role, text: m.text, source_line: m.source_line, source_name: m.source_name });
    }
  }
  return cards;
};