import { mkdirSync, copyFileSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const root = dirname(fileURLToPath(import.meta.url));
const repo = join(root, '..');
const dist = join(repo, 'site-dist');

const allowlist = [
  'site/index.html',
  'site/art-hero.webp',
  'site/plane.html',
  'site/ledger.html',
  'site/faq.html',
  'site/art-hero.jpg',
  'site/art-night.jpg',
  'site/art-dawn.jpg',
  'site/art-enso.png',
  'site/art-faq.jpg',
  'site/robots.txt',
  'site/sitemap.xml'
];

import { existsSync } from 'node:fs';
for (const file of allowlist) {
  if (!existsSync(join(repo, file))) {
    throw new Error(`Missing required file: ${file}`);
  }
}

mkdirSync(dist, { recursive: true });
for (const file of allowlist) {
  copyFileSync(join(repo, file), join(dist, file.replace(/^site\//, '')));
}
console.log(`Copied ${allowlist.length} files to site-dist`);
