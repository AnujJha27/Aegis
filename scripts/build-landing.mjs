import { mkdirSync, copyFileSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const root = dirname(fileURLToPath(import.meta.url));
const repo = join(root, '..');
const dist = join(repo, 'site-dist');

const allowlist = [
  'index.html',
  'plane.html',
  'ledger.html',
  'faq.html',
  'art-hero.jpg',
  'art-night.jpg',
  'art-dawn.jpg',
  'art-enso.png',
  'art-faq.jpg',
  'robots.txt',
  'sitemap.xml'
];

import { existsSync } from 'node:fs';
for (const file of allowlist) {
  if (!existsSync(join(repo, file))) {
    throw new Error(`Missing required file: ${file}`);
  }
}

mkdirSync(dist, { recursive: true });
for (const file of allowlist) {
  copyFileSync(join(repo, file), join(dist, file));
}
console.log(`Copied ${allowlist.length} files to site-dist`);
