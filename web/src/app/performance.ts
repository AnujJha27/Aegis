export type PerfSample = { name: string; ms: number };
const samples = new Map<string, number[]>();
let enabled = false;
export function enablePerformanceRecording() {
  if (enabled) return;
  enabled = true;
  window.addEventListener("aegis:perf", (event) => {
    const { name, ms } = (event as CustomEvent<PerfSample>).detail;
    if (!["quick-open", "file-first-rows", "diff-first-content", "diff-first-hunk", "scroll-frame"].includes(name) || !Number.isFinite(ms)) return;
    const values = samples.get(name) ?? [];
    values.push(ms); if (values.length > 200) values.shift();
    samples.set(name, values);
  });
}
export function reviewPerformance() {
  return Object.fromEntries([...samples].map(([name, values]) => {
    const ordered = [...values].sort((a, b) => a - b);
    return [name, { samples: ordered.length, p50: ordered[Math.floor(ordered.length * .5)], p95: ordered[Math.min(ordered.length - 1, Math.floor(ordered.length * .95))] }];
  }));
}
