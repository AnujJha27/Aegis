// Check the production lazy chunk and render with the pinned Mermaid package; jsdom supplies DOM/layout stubs, not browser timing.
import { readdir } from 'node:fs/promises';
import assert from 'node:assert/strict';
import { JSDOM } from '../web/node_modules/jsdom/lib/api.js';
const window = new JSDOM('<!doctype html><html><body></body></html>', { url: 'http://127.0.0.1/' }).window;
Object.assign(globalThis, { window, document: window.document, DOMParser: window.DOMParser, XMLSerializer: window.XMLSerializer, Element: window.Element, SVGElement: window.SVGElement, CSSStyleSheet: window.CSSStyleSheet, HTMLElement: window.HTMLElement, Node: window.Node, getComputedStyle: window.getComputedStyle });
Object.defineProperty(window.SVGElement.prototype, 'getBBox', { value: () => ({ x: 0, y: 0, width: 100, height: 30 }) });
Object.defineProperty(window.SVGElement.prototype, 'getComputedTextLength', { value: () => 100 });
const files = await readdir(new URL('../web/dist/assets/', import.meta.url));
const name = files.find((file) => file.startsWith('mermaid.core-') && file.endsWith('.js'));
assert(name, 'Mermaid must be a separate lazy chunk');
const mermaid = (await import("../web/node_modules/mermaid/dist/mermaid.core.mjs")).default;
const { sanitizeSvg } = await import('../web/src/features/markdown/security.ts');
mermaid.initialize({ startOnLoad: false, securityLevel: 'strict', htmlLabels: false, suppressErrorRendering: true, flowchart: { htmlLabels: false } });
for (const [i, source] of ['flowchart LR\nA[Review] --> B[Verify]', 'sequenceDiagram\nReviewer->>Daemon: Read file'].entries()) {
  const rendered = await mermaid.render(`smoke-${i}`, source);
  const safe = sanitizeSvg(rendered.svg);
  assert(safe.includes('<svg')); assert(!/<script|onclick=|onload=|foreignObject/.test(safe));
}
console.log('Mermaid lazy chunk present; real flowchart/sequence render and SVG boundary passed.');
