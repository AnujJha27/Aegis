// Node 24's native TypeScript stripping runs the same line/diff algorithms used by the browser.
import { performance } from 'node:perf_hooks';
import { indexLines } from '../web/src/features/code/lines.ts';
import { diffHunks } from '../web/src/features/code/diff.ts';
let source = 'const review = true;\n'.repeat(400000);
let start = performance.now();
const lines = indexLines(source);
console.log(JSON.stringify({ largeFileBytes: source.length, lines: lines.count, indexMs: performance.now() - start }));
source = '';
for (let pass = 0; pass < 4; pass++) {
  for (let file = 0; file < 300; file++) {
    const text = `file ${pass}-${file}\n` + 'review source\n'.repeat(8000);
    indexLines(text);
    if (file % 10 === 0) diffHunks(text, text.replace('review', 'changed'));
  }
  globalThis.gc?.();
  console.log(JSON.stringify({ pass, heapMiB: process.memoryUsage().heapUsed / 1048576, rssMiB: process.memoryUsage().rss / 1048576 }));
}
