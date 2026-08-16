#!/usr/bin/env node
/**
 * Generate the browser wrapper page for every scene.
 *
 * Generated rather than hand-written so all scenes are byte-identical apart from the
 * module path. A hand-edited page that drifts (a different canvas size, a stray CSS
 * transform) would show up as a differ mismatch attributed to the runtime.
 */
import { readdirSync, writeFileSync, existsSync, statSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const __dirname = dirname(fileURLToPath(import.meta.url));
const SCENES = join(__dirname, '..', 'scenes');

const page = (scene) => `<!doctype html>
<html>
<head>
<meta charset="utf-8">
<title>bench: ${scene}</title>
<style>
  /* No scaling, no smoothing, no device-pixel-ratio games: the canvas backing store
     must be exactly width x height so captures are comparable to the launchers. */
  html, body { margin: 0; padding: 0; background: #0d1014; overflow: hidden; }
  canvas { display: block; image-rendering: pixelated; }
</style>
</head>
<body>
<canvas id="game-canvas" width="960" height="540"></canvas>
<script type="module">
  const p = new URLSearchParams(location.search);
  const c = document.getElementById('game-canvas');
  c.width = Number(p.get('width') || 960);
  c.height = Number(p.get('height') || 540);
  import('./main.js').catch((e) => {
    document.title = 'ERROR';
    window.__benchResult = { error: String(e && e.stack || e) };
    console.error(e);
  });
</script>
</body>
</html>
`;

let n = 0;
for (const entry of readdirSync(SCENES)) {
  const dir = join(SCENES, entry);
  if (!statSync(dir).isDirectory() || entry === 'lib') continue;
  if (!existsSync(join(dir, 'main.js'))) continue;
  writeFileSync(join(dir, 'index.html'), page(entry));
  n++;
}
console.log(`generated ${n} scene pages`);

/* Also emit a package.json per scene so the launchers resolve `main.js` explicitly
   rather than relying on their fallback probe order. */
import { writeFileSync as wfs } from 'node:fs';
for (const entry of readdirSync(SCENES)) {
  const dir = join(SCENES, entry);
  if (!statSync(dir).isDirectory() || entry === 'lib') continue;
  if (!existsSync(join(dir, 'main.js'))) continue;
  wfs(join(dir, 'package.json'), JSON.stringify({
    name: `bench-${entry}`, version: '0.0.0', type: 'module', main: 'main.js',
  }, null, 2) + '\n');
}
