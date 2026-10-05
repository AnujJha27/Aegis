export type Lines = { source: string; offsets: Uint32Array; count: number; maxColumns: number; line: (index: number) => string };
const recent = new Map<string, Lines>();
let bytes = 0;
export function indexLines(source: string): Lines {
  const cached = recent.get(source);
  if (cached) { recent.delete(source); recent.set(source, cached); return cached; }
  let count = 1;
  for (let i = 0; i < source.length; i++) if (source.charCodeAt(i) === 10) count++;
  const offsets = new Uint32Array(count + 1);
  let row = 1, width = 0, maxColumns = 0;
  for (let i = 0; i < source.length; i++) {
    if (source.charCodeAt(i) === 10) { offsets[row++] = i + 1; maxColumns = Math.max(maxColumns, width); width = 0; }
    else width += source[i] === "\t" ? 4 : 1;
  }
  maxColumns = Math.max(maxColumns, width);
  offsets[count] = source.length + 1;
  const result: Lines = { source, offsets, count, maxColumns, line: (i) => source.slice(offsets[i], Math.min(source.length, offsets[i + 1] - 1)).replace(/\r$/, "") };
  const cost = source.length * 2 + offsets.byteLength;
  if (cost <= 32 * 1024 * 1024) {
    while (recent.size >= 12 || bytes + cost > 32 * 1024 * 1024) {
      const key = recent.keys().next().value!; const old = recent.get(key)!;
      bytes -= old.source.length * 2 + old.offsets.byteLength; recent.delete(key);
    }
    recent.set(source, result); bytes += cost;
  }
  return result;
}
export function visibleRange(top: number, height: number, count: number, lineHeight = 22) {
  const first = Math.max(0, Math.floor(top / lineHeight) - 12);
  return { first: Math.min(first, Math.max(0, count - 1)), end: Math.min(count, first + Math.ceil(height / lineHeight) + 24) };
}
