import { readdir, readFile } from 'node:fs/promises';
import { gzipSync } from 'node:zlib';
const root = new URL('../web/dist/', import.meta.url);
const html = await readFile(new URL('index.html', root), 'utf8');
for (const file of (await readdir(new URL('assets/', root))).filter((name) => name.endsWith('.js')).sort()) {
  const bytes = await readFile(new URL(`assets/${file}`, root));
  console.log(`${file}\traw=${bytes.length}\tgzip=${gzipSync(bytes).length}\t${html.includes(file) ? 'entry' : 'lazy/shared'}`);
}
