export type DiffHunk = { old: number; next: number; kind: "equal" | "add" | "delete" | "modify"; count: number };
export type DiffRow = { old: number; next: number; kind: "equal" | "add" | "delete" | "modify" };
// ponytail: patience anchors keep time/memory bounded; repeated unmatched blocks may be non-minimal. Use bounded Myers in gaps if review fixtures justify it.
export function diffHunks(original: string, modified: string): DiffHunk[] {
  const a = original ? original.split("\n") : [], b = modified ? modified.split("\n") : [];
  const rows: DiffHunk[] = [];
  function append(row: DiffRow) {
    const last = rows.at(-1);
    if (last && last.kind === row.kind && (last.old < 0 ? row.old < 0 : row.old === last.old + last.count) && (last.next < 0 ? row.next < 0 : row.next === last.next + last.count)) last.count++;
    else rows.push({ ...row, count: 1 });
  }
  let prefix = 0;
  while (prefix < a.length && prefix < b.length && a[prefix] === b[prefix]) { append({ old: prefix, next: prefix, kind: "equal" }); prefix++; }
  let ae = a.length, be = b.length;
  while (ae > prefix && be > prefix && a[ae - 1] === b[be - 1]) { ae--; be--; }
  const oldUnique = new Map<string, number>(), newUnique = new Map<string, number>();
  for (let i = prefix; i < ae; i++) oldUnique.set(a[i], oldUnique.has(a[i]) ? -1 : i);
  for (let i = prefix; i < be; i++) newUnique.set(b[i], newUnique.has(b[i]) ? -1 : i);
  const pairs: [number, number][] = [];
  for (let i = prefix; i < ae; i++) { const j = newUnique.get(a[i]); if (oldUnique.get(a[i]) === i && j !== undefined && j >= 0) pairs.push([i, j]); }
  const tails: number[] = [], previous = new Int32Array(pairs.length).fill(-1);
  for (let i = 0; i < pairs.length; i++) {
    let low = 0, high = tails.length;
    while (low < high) { const mid = (low + high) >>> 1; if (pairs[tails[mid]][1] < pairs[i][1]) low = mid + 1; else high = mid; }
    if (low) previous[i] = tails[low - 1]; tails[low] = i;
  }
  const anchors: [number, number][] = [];
  for (let i = tails.at(-1) ?? -1; i >= 0; i = previous[i]) anchors.push(pairs[i]);
  anchors.reverse(); anchors.push([ae, be]);
  let old = prefix, next = prefix;
  for (const [oa, nb] of anchors) {
    while (old < oa || next < nb) {
      const hasOld = old < oa, hasNew = next < nb;
      append({ old: hasOld ? old++ : -1, next: hasNew ? next++ : -1, kind: hasOld && hasNew ? "modify" : hasOld ? "delete" : "add" });
    }
    if (oa < ae) { append({ old: old++, next: next++, kind: "equal" }); }
  }
  while (old < a.length && next < b.length) append({ old: old++, next: next++, kind: "equal" });
  return rows;
}
export function diffLayout(hunks: DiffHunk[], inline: boolean) {
  const visible = inline ? hunks.flatMap((h) => h.kind === "modify" ? [{ ...h, next: -1, kind: "delete" as const }, { ...h, old: -1, kind: "add" as const }] : [h]) : hunks;
  let count = 0;
  const starts = visible.map((hunk) => { const first = count; count += hunk.count; return first; });
  const changes = starts.filter((_, i) => visible[i].kind !== "equal" && (i === 0 || visible[i - 1].kind === "equal"));
  return { count, changes, row: (index: number): DiffRow => {
    let low = 0, high = starts.length;
    while (low + 1 < high) { const mid = (low + high) >>> 1; if (starts[mid] <= index) low = mid; else high = mid; }
    const hunk = visible[low], offset = index - starts[low];
    return { old: hunk.old < 0 ? -1 : hunk.old + offset, next: hunk.next < 0 ? -1 : hunk.next + offset, kind: hunk.kind };
  } };
}
