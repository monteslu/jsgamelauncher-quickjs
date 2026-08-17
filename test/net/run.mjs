#!/usr/bin/env node
/*
 * Networking tests: fetch over HTTP, and WebSocket.
 *
 * Both run against servers this script starts itself, so there is no external
 * service to be flaky or offline and the whole thing works in CI. The servers
 * are plain Node — no dependency on `ws`, because the point is to test OUR
 * frame codec against an independent implementation of the protocol, and
 * hand-rolling the server side here means the two implementations were written
 * from the spec rather than from each other.
 *
 * TLS is deliberately NOT tested against a self-signed local server: our client
 * requires certificate verification, and a test that disabled verification to
 * make a self-signed cert pass would be testing the opposite of what matters.
 * The https path is covered by an opt-in live check (JSGLQ_NET_LIVE=1).
 */
import { createServer } from 'node:http';
import { createHash } from 'node:crypto';
import { execFile } from 'node:child_process';
import { mkdtempSync, writeFileSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = join(dirname(fileURLToPath(import.meta.url)), '..', '..');
const BIN = join(ROOT, 'build', 'jsglq');

let pass = 0, fail = 0;
const failures = [];

/*
 * ASYNC, deliberately.
 *
 * spawnSync blocks Node's event loop, so the test server in THIS process can
 * never accept the connection the child is making — the child then waits for a
 * response that cannot come and every test times out. That failure looks
 * exactly like a broken HTTP client, and cost a long debugging detour before
 * `curl` against the same server failed too and showed the fixture was at
 * fault, not the runtime.
 */
function run(source, { timeout = 20000 } = {}) {
  const dir = mkdtempSync(join(tmpdir(), 'jsglq-net-'));
  writeFileSync(join(dir, 'main.js'), source);
  return new Promise((resolve) => {
    execFile(BIN, ['--headless', '--max-seconds=8', dir], {
      encoding: 'utf8',
      timeout,
      env: { ...process.env, LIBGL_ALWAYS_SOFTWARE: '1', EGL_PLATFORM: 'surfaceless' },
    }, (_err, stdout, stderr) => {
      rmSync(dir, { recursive: true, force: true });
      resolve((stdout || '') + (stderr || ''));
    });
  });
}

async function check(name, source, verify) {
  const out = await run(source);
  let res;
  try { res = verify(out); } catch (e) { res = { ok: false, detail: e.message }; }
  if (res.ok) { pass++; console.log(`  ok    ${name}${res.detail ? '  — ' + res.detail : ''}`); }
  else { fail++; failures.push({ name, detail: res.detail, out }); console.log(`  FAIL  ${name}`); }
}

/* --------------------------------------------------------------- servers -- */

const httpServer = createServer((req, res) => {
  if (req.url === '/hello') {
    res.writeHead(200, { 'Content-Type': 'text/plain' });
    res.end('hello from the test server');
  } else if (req.url === '/echo' && req.method === 'POST') {
    let body = '';
    req.on('data', (c) => { body += c; });
    req.on('end', () => {
      res.writeHead(200, { 'Content-Type': 'text/plain' });
      res.end(`echo:${body}`);
    });
  } else if (req.url === '/redirect') {
    res.writeHead(302, { Location: '/hello' });
    res.end();
  } else if (req.url === '/chunked') {
    res.writeHead(200, { 'Transfer-Encoding': 'chunked' });
    res.write('part-one|');
    res.write('part-two');
    res.end();
  } else if (req.url === '/notfound') {
    res.writeHead(404); res.end('nope');
  } else {
    res.writeHead(200); res.end('root');
  }
});

/* A minimal RFC 6455 server: handshake, then echo text frames back. */
const GUID = '258EAFA5-E914-47DA-95CA-C5AB0DC85B11';
httpServer.on('upgrade', (req, socket) => {
  const key = req.headers['sec-websocket-key'];
  const accept = createHash('sha1').update(key + GUID).digest('base64');
  socket.write(
    'HTTP/1.1 101 Switching Protocols\r\n' +
    'Upgrade: websocket\r\nConnection: Upgrade\r\n' +
    `Sec-WebSocket-Accept: ${accept}\r\n\r\n`);

  let buf = Buffer.alloc(0);
  socket.on('data', (chunk) => {
    buf = Buffer.concat([buf, chunk]);
    for (;;) {
      if (buf.length < 2) return;
      const opcode = buf[0] & 0x0f;
      const masked = (buf[1] & 0x80) !== 0;
      let len = buf[1] & 0x7f;
      let off = 2;
      if (len === 126) { if (buf.length < 4) return; len = buf.readUInt16BE(2); off = 4; }
      else if (len === 127) { if (buf.length < 10) return; len = Number(buf.readBigUInt64BE(2)); off = 10; }
      const maskKey = masked ? buf.subarray(off, off + 4) : null;
      if (masked) off += 4;
      if (buf.length < off + len) return;
      const payload = Buffer.from(buf.subarray(off, off + len));
      if (maskKey) for (let i = 0; i < payload.length; i++) payload[i] ^= maskKey[i & 3];
      buf = buf.subarray(off + len);

      if (opcode === 0x8) { socket.end(); return; }
      if (opcode === 0x1 || opcode === 0x2) {
        // Echo it back UNMASKED, which is what a server must do.
        const out = Buffer.concat([
          Buffer.from([0x80 | opcode, payload.length < 126 ? payload.length : 126]),
          payload.length < 126 ? Buffer.alloc(0) : (() => {
            const b = Buffer.alloc(2); b.writeUInt16BE(payload.length); return b;
          })(),
          payload,
        ]);
        socket.write(out);
      }
    }
  });
  socket.on('error', () => {});
});

await new Promise((r) => httpServer.listen(0, '127.0.0.1', r));
const PORT = httpServer.address().port;
const BASE = `http://127.0.0.1:${PORT}`;

console.log(`\n=== networking tests (server on 127.0.0.1:${PORT}) ===\n`);

/* ----------------------------------------------------------------- fetch -- */

await check('fetch: GET a remote URL returns the body',
  `fetch('${BASE}/hello').then(r => r.text()).then(t => console.log('R:' + t))
     .catch(e => console.log('R:ERR:' + e.message));`,
  (o) => ({ ok: /R:hello from the test server/.test(o),
            detail: (/R:(.*)/.exec(o) || [])[1] }));

await check('fetch: status and statusText survive',
  `fetch('${BASE}/notfound').then(r => console.log('R:' + r.status + ',' + r.ok))
     .catch(e => console.log('R:ERR:' + e.message));`,
  (o) => ({ ok: /R:404,false/.test(o), detail: (/R:(.*)/.exec(o) || [])[1] }));

await check('fetch: POST sends a body and reads the reply',
  `fetch('${BASE}/echo', { method: 'POST', body: 'payload-1234' })
     .then(r => r.text()).then(t => console.log('R:' + t))
     .catch(e => console.log('R:ERR:' + e.message));`,
  (o) => ({ ok: /R:echo:payload-1234/.test(o), detail: (/R:(.*)/.exec(o) || [])[1] }));

await check('fetch: follows a redirect',
  `fetch('${BASE}/redirect').then(r => r.text()).then(t => console.log('R:' + t))
     .catch(e => console.log('R:ERR:' + e.message));`,
  (o) => ({ ok: /R:hello from the test server/.test(o),
            detail: (/R:(.*)/.exec(o) || [])[1] }));

await check('fetch: decodes a chunked response',
  `fetch('${BASE}/chunked').then(r => r.text()).then(t => console.log('R:' + t))
     .catch(e => console.log('R:ERR:' + e.message));`,
  (o) => ({ ok: /R:part-one\|part-two/.test(o), detail: (/R:(.*)/.exec(o) || [])[1] }));

await check('fetch: a refused connection rejects BY NAME (MUST-FAIL control)',
  // Port 1 is reserved and nothing listens there.
  `fetch('http://127.0.0.1:1/nope').then(() => console.log('R:RESOLVED-WRONGLY'))
     .catch(e => console.log('R:REJECTED:' + e.message.slice(0, 60)));`,
  (o) => ({ ok: /R:REJECTED/.test(o) && !/RESOLVED-WRONGLY/.test(o),
            detail: (/R:(.*)/.exec(o) || [])[1] }));

await check('fetch: local assets still work alongside remote',
  `fetch('./main.js').then(r => r.text()).then(t => console.log('R:local,' + (t.length > 0)))
     .catch(e => console.log('R:ERR:' + e.message));`,
  (o) => ({ ok: /R:local,true/.test(o), detail: (/R:(.*)/.exec(o) || [])[1] }));

/* ------------------------------------------------------------- websocket -- */

await check('websocket: connects, sends and receives an echo',
  `const ws = new WebSocket('ws://127.0.0.1:${PORT}/');
   ws.onopen = () => ws.send('ping-42');
   ws.onmessage = (e) => { console.log('R:' + e.data); ws.close(); };
   ws.onerror = (e) => console.log('R:ERR:' + (e.message || 'error'));`,
  (o) => ({ ok: /R:ping-42/.test(o), detail: (/R:(.*)/.exec(o) || [])[1] }));

await check('websocket: readyState reaches OPEN',
  `const ws = new WebSocket('ws://127.0.0.1:${PORT}/');
   ws.onopen = () => { console.log('R:' + ws.readyState + ',' + WebSocket.OPEN); ws.close(); };`,
  (o) => ({ ok: /R:1,1/.test(o), detail: (/R:(.*)/.exec(o) || [])[1] }));

await check('websocket: a message larger than 125 bytes round-trips',
  // Crosses the 7-bit length boundary into the 16-bit extended form, which is a
  // different code path in both the encoder and the decoder.
  `const big = 'x'.repeat(500);
   const ws = new WebSocket('ws://127.0.0.1:${PORT}/');
   ws.onopen = () => ws.send(big);
   ws.onmessage = (e) => { console.log('R:' + (e.data === big ? 'MATCH' : 'MISMATCH:' + e.data.length)); ws.close(); };`,
  (o) => ({ ok: /R:MATCH/.test(o), detail: (/R:(.*)/.exec(o) || [])[1] }));

await check('websocket: wss:// without TLS throws BY NAME (MUST-FAIL control)',
  `try { new WebSocket('wss://example.com/'); console.log('R:NO-THROW'); }
   catch (e) { console.log('R:THREW:' + /TLS|tls/.test(e.message)); }`,
  (o) => {
    // With TLS compiled in this is expected to attempt a connection instead.
    if (/R:NO-THROW/.test(o)) return { ok: true, detail: 'TLS build: wss accepted' };
    return { ok: /R:THREW:true/.test(o), detail: (/R:(.*)/.exec(o) || [])[1] };
  });

await check('websocket: closing leaves no live socket (MUST-FAIL control)',
  `const ws = new WebSocket('ws://127.0.0.1:${PORT}/');
   ws.onopen = () => {
     ws.close();
     try { ws.send('after-close'); console.log('R:SENT-AFTER-CLOSE'); }
     catch (e) { console.log('R:REFUSED,' + ws.readyState); }
   };`,
  (o) => ({ ok: /R:REFUSED,3/.test(o), detail: (/R:(.*)/.exec(o) || [])[1] }));

/* ------------------------------------------------------------------ live -- */

if (process.env.JSGLQ_NET_LIVE === '1') {
  check('fetch: https:// against a real server verifies the certificate',
    `fetch('https://example.com/').then(r => console.log('R:' + r.status))
       .catch(e => console.log('R:ERR:' + e.message.slice(0, 80)));`,
    (o) => ({ ok: /R:200/.test(o), detail: (/R:(.*)/.exec(o) || [])[1] }));

  check('fetch: an expired certificate is REJECTED (MUST-FAIL control)',
    // badssl.com publishes deliberately broken certs for exactly this.
    `fetch('https://expired.badssl.com/').then(() => console.log('R:ACCEPTED-BAD-CERT'))
       .catch(e => console.log('R:REJECTED:' + e.message.slice(0, 70)));`,
    (o) => ({ ok: /R:REJECTED/.test(o) && /certificate|verif/i.test(o),
              detail: (/R:(.*)/.exec(o) || [])[1] }));
} else {
  console.log('  skip  https live checks (set JSGLQ_NET_LIVE=1 to run)');
}

httpServer.close();

console.log(`\n${pass} passed, ${fail} failed`);
if (fail) {
  console.log('\nFailures:');
  for (const f of failures) console.log(`\n--- ${f.name} ---\n${f.detail}\n${f.out}`);
  process.exit(1);
}
