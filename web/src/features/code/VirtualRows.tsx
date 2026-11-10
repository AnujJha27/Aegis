import { useEffect, useLayoutEffect, useRef, useState, type ReactNode } from "react";
import { visibleRange } from "./lines";
const positions = new Map<string, number>();
export function VirtualRows({ count, width, identity, renderRow, jump = 0, copy, copyRows }: { count: number; width: number; identity: string; renderRow: (i: number) => ReactNode; jump?: number; copy: () => string; copyRows?: (first: number, end: number) => string }) {
  const viewport = useRef<HTMLDivElement>(null);
  const frame = useRef(0);
  const [window, setWindow] = useState({ first: 0, end: Math.min(count, 60) });
  const selectAll = useRef(false);
  const selectedRange = useRef<{ anchor: number; end: number } | undefined>(undefined);
  const [current, setCurrent] = useState(0);
  const update = () => {
    const node = viewport.current;
    if (!node) return;
    const next = visibleRange(node.scrollTop, node.clientHeight || 600, count);
    setWindow((old) => old.first === next.first && old.end === next.end ? old : next);
  };
  useLayoutEffect(() => {
    if (viewport.current) viewport.current.scrollTop = positions.get(identity) ?? 0;
    update();
    const observer = typeof ResizeObserver === "undefined" ? undefined : new ResizeObserver(update);
    if (viewport.current) observer?.observe(viewport.current);
    return () => { observer?.disconnect(); cancelAnimationFrame(frame.current); };
  }, [identity, count]);
  useEffect(() => {
    if (jump && viewport.current) { viewport.current.scrollTop = Math.max(0, jump - 1) * 22; setCurrent(jump - 1); update(); }
  }, [jump]);
  const rows = [];
  const safeFirst = Math.max(0, Math.min(window.first, count));
  const safeEnd = Math.max(safeFirst, Math.min(window.end, count));
  for (let i = safeFirst; i < safeEnd; i++) rows.push(<div key={i} data-row={i} className={`virtual-row ${i === current ? "current" : ""}`} style={{ height: 22 }} onMouseDown={(event) => {
    selectAll.current = false;
    if (event.shiftKey && selectedRange.current) selectedRange.current.end = i;
    else selectedRange.current = { anchor: i, end: i };
    setCurrent(i);
  }}>{renderRow(i)}</div>);
  return <div className="virtual-viewport" tabIndex={0} aria-label="Read-only code" ref={viewport}
    onScroll={() => {
      if (frame.current) return;
      const started = performance.now();
      frame.current = requestAnimationFrame(() => {
        frame.current = 0; update();
        positions.delete(identity); positions.set(identity, viewport.current?.scrollTop ?? 0);
        if (positions.size > 32) positions.delete(positions.keys().next().value!);
        windowDispatch("scroll-frame", performance.now() - started);
      });
    }} onCopy={(event) => {
      if (selectAll.current) { event.preventDefault(); event.clipboardData.setData("text/plain", copy()); }
      else if (selectedRange.current && selectedRange.current.anchor !== selectedRange.current.end) {
        const { anchor, end } = selectedRange.current;
        event.preventDefault(); event.clipboardData.setData("text/plain", copyRows ? copyRows(Math.min(anchor, end), Math.max(anchor, end) + 1) : copy().split("\n").slice(Math.min(anchor, end), Math.max(anchor, end) + 1).join("\n"));
      }
    }} onKeyDown={(event) => {
      if ((event.ctrlKey || event.metaKey) && event.key === "a") { event.preventDefault(); selectAll.current = true; return; }
      const page = Math.floor((viewport.current?.clientHeight || 600) / 22);
      const delta = ({ ArrowDown: 1, ArrowUp: -1, PageDown: page, PageUp: -page } as Record<string, number>)[event.key];
      if (delta === undefined && event.key !== "Home" && event.key !== "End") return;
      event.preventDefault();
      const next = Math.max(0, Math.min(count - 1, event.key === "Home" ? 0 : event.key === "End" ? count - 1 : current + delta));
      setCurrent(next);
      if (viewport.current) { viewport.current.scrollTop = Math.max(0, next - 3) * 22; update(); }
    }}>
    <div style={{ height: count * 22, minWidth: Math.max(width, 100), position: "relative" }}>
      <div style={{ position: "absolute", top: window.first * 22, left: 0, right: 0 }}>{rows}</div>
    </div>
  </div>;
}
function windowDispatch(name: string, ms: number) { globalThis.window.dispatchEvent(new CustomEvent("aegis:perf", { detail: { name, ms } })); }
