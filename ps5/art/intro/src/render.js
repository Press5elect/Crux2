// Crux2 - the startup video (ps5/art/intro, README.md there): renders main.js frame by frame in Chromium
// (Playwright) into ffmpeg. Made from OGBox's render.js (OGBox by Press5elect).
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
const { chromium } = require('playwright');
const http = require('http'), fs = require('fs'), path = require('path'), { spawn } = require('child_process');
const root = __dirname, PORT = 8765, FPS = 60;
const mime = { '.html': 'text/html', '.js': 'text/javascript', '.png': 'image/png' };
const srv = http.createServer((q, s) => { const f = path.join(root, decodeURIComponent(q.url.split('?')[0])); fs.readFile(f, (e, d) => { if (e) { s.writeHead(404); s.end(); } else { s.writeHead(200, { 'content-type': mime[path.extname(f)] || 'application/octet-stream' }); s.end(d); } }); }).listen(PORT);
(async () => {
  const b = await chromium.launch({ args: ['--use-angle=swiftshader', '--enable-unsafe-swiftshader', '--ignore-gpu-blocklist'] });
  const p = await b.newPage({ viewport: { width: 1280, height: 720 }, deviceScaleFactor: 1 });
  p.on('console', m => console.log('page:', m.text())); p.on('pageerror', e => console.log('pageerror:', e.message));
  await p.goto(`http://localhost:${PORT}/index.html`);
  await p.waitForFunction('window.ready === true', null, { timeout: 60000 });
  const T = await p.evaluate('window.T'); console.log('cells', await p.evaluate('window.NCELLS'));
  const shot = async t => Buffer.from((await p.evaluate(([t, f]) => { window.renderFrame(t, f); return window.grab(); }, [t, FPS])).split(',')[1], 'base64');
  if (process.argv[2] === 'stills') {
    for (const t of process.argv.slice(3).map(Number)) { const t0 = Date.now(); fs.writeFileSync(path.join(root, `s_${t.toFixed(2)}.png`), await shot(t)); console.log(t, Date.now() - t0, 'ms'); }
  } else {
    const out = process.argv[2] || 'video.mp4', n = Math.round(T.end * FPS);
    const ff = spawn('ffmpeg', ['-y', '-loglevel', 'error', '-f', 'image2pipe', '-framerate', String(FPS), '-c:v', 'png', '-i', '-', '-c:v', 'libx264', '-preset', 'slow', '-crf', '14', '-pix_fmt', 'yuv420p', out], { stdio: ['pipe', 'inherit', 'inherit'] });
    const t0 = Date.now();
    for (let i = 0; i < n; i++) { const png = await shot(i / FPS); if (!ff.stdin.write(png)) await new Promise(r => ff.stdin.once('drain', r)); if (i % 30 === 0) console.log(i, '/', n, ((Date.now() - t0) / 1000).toFixed(0), 's'); }
    ff.stdin.end(); await new Promise(r => ff.on('close', r));
  }
  await b.close(); srv.close();
})();
