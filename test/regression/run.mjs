#!/usr/bin/env node
/**
 * Regression suite.
 *
 * Every check here corresponds to a bug that was actually found and fixed during
 * bring-up, or to a behaviour that a reference runtime gets wrong. House rules from
 * the plan, all of which apply:
 *
 *  - each check must have been verified RED before the fix (noted per case)
 *  - checks read pixels through the shipped path, not a private one
 *  - colour counts are liveness signals, never fixtures
 *  - some checks are MUST-FAIL controls: if they ever pass, the harness is broken
 */
import { spawnSync } from 'node:child_process';
import { mkdtempSync, writeFileSync, mkdirSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join, dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const __dirname = dirname(fileURLToPath(import.meta.url));
const BIN = process.env.JSGLQ_BIN || resolve(__dirname, '..', '..', 'build', 'jsglq');

let pass = 0, fail = 0;
const failures = [];

/** Run a game whose main.js is `src`; return its stdout+stderr. */
function run(src, { frames = 20, files = {} } = {}) {
  const dir = mkdtempSync(join(tmpdir(), 'jsglq-reg-'));
  try {
    writeFileSync(join(dir, 'main.js'), src);
    for (const [name, content] of Object.entries(files)) {
      const p = join(dir, name);
      mkdirSync(dirname(p), { recursive: true });
      writeFileSync(p, content);
    }
    const r = spawnSync(BIN, ['--headless', `--frames=${frames}`, dir],
                        { encoding: 'utf8', timeout: 60000 });
    return (r.stdout || '') + (r.stderr || '');
  } finally {
    rmSync(dir, { recursive: true, force: true });
  }
}

function check(name, src, assertion, opts) {
  const out = run(src, opts);
  let ok = false, detail = '';
  try {
    const res = assertion(out);
    ok = res === true || (res && res.ok);
    detail = (res && res.detail) || '';
  } catch (err) {
    ok = false;
    detail = err.message;
  }
  if (ok) { pass++; console.log(`  ok    ${name}${detail ? '  — ' + detail : ''}`); }
  else {
    fail++;
    failures.push({ name, detail, out: out.slice(-600) });
    console.log(` FAIL   ${name}${detail ? '  — ' + detail : ''}`);
  }
}

console.log('=== jsgamelauncher-quickjs regression suite ===\n');

/* ---------------------------------------------------------------- sandbox ---- */

check('sandbox: vm-escape one-liner is blocked',
  `console.log('R:' + (function(){ try {
     return typeof this.constructor.constructor('return process')();
   } catch(e){ return 'blocked'; } })());`,
  (o) => {
    // This exact expression walks out of rungame's node:vm realm. Here there is no
    // host realm behind the wall, so it must not resolve to anything.
    const m = /R:(\w+)/.exec(o);
    return { ok: m && (m[1] === 'blocked' || m[1] === 'undefined'),
             detail: m ? m[1] : 'no result' };
  });

check('sandbox: no fs/process/require reachable',
  `console.log('R:' + [typeof process, typeof require, typeof globalThis.std,
                       typeof globalThis.os].join(','));`,
  (o) => ({ ok: /R:undefined,undefined,undefined,undefined/.test(o),
            detail: (/R:(\S+)/.exec(o) || [])[1] }));

/* -------------------------------------------------------------------- rAF ---- */

check('rAF fires ALL callbacks registered for a frame (rungame drops all but last)',
  `let a=0,b=0,c=0,n=0;
   function tick(){ n++;
     if(n<=3){ requestAnimationFrame(()=>a++); requestAnimationFrame(()=>b++);
               requestAnimationFrame(()=>{c++; if(c===3) console.log('R:'+a+','+b+','+c);});
               requestAnimationFrame(tick); } }
   requestAnimationFrame(tick);`,
  (o) => ({ ok: /R:3,3,3/.test(o), detail: (/R:(\S+)/.exec(o) || [])[1] }));

check('rAF callbacks registered during a frame run on the NEXT frame',
  `let inner=0, outer=0;
   requestAnimationFrame(() => { outer++; requestAnimationFrame(() => { inner++; }); });
   let n=0; (function loop(){ if(++n<6) requestAnimationFrame(loop);
     else console.log('R:'+outer+','+inner); })();`,
  (o) => {
    // If registration-during-dispatch ran in the same frame this would spin.
    const m = /R:(\d+),(\d+)/.exec(o);
    return { ok: m && m[1] === '1' && m[2] === '1', detail: m ? m[0] : 'no result' };
  }, { frames: 30 });

/* --------------------------------------------------------------- canvas2d ---- */

check('canvas2d: orientation is top-left (getImageData row order)',
  `const c=document.getElementById('game-canvas'); const x=c.getContext('2d');
   let n=0;
   (function f(){
     x.fillStyle='#000'; x.fillRect(0,0,c.width,c.height);
     x.fillStyle='red';    x.fillRect(0,0,16,16);
     x.fillStyle='lime';   x.fillRect(c.width-16,0,16,16);
     x.fillStyle='blue';   x.fillRect(0,c.height-16,16,16);
     x.fillStyle='yellow'; x.fillRect(c.width-16,c.height-16,16,16);
     if(++n===2){
       const p=(px,py)=>{const d=x.getImageData(px,py,1,1).data;return d[0]+','+d[1]+','+d[2];};
       console.log('R:'+[p(4,4),p(c.width-5,4),p(4,c.height-5),p(c.width-5,c.height-5)].join('|'));
     }
     if(n<4) requestAnimationFrame(f);
   })();`,
  (o) => {
    // WAS RED: glReadPixels returns rows bottom-up, so the whole image was
    // mirrored while every colour count stayed identical.
    const m = /R:(\S+)/.exec(o);
    return { ok: m && m[1] === '255,0,0|0,255,0|0,0,255|255,255,0',
             detail: m ? m[1] : 'no result' };
  });

check('canvas2d: putImageData round-trips through getImageData (same frame)',
  `const c=document.getElementById('game-canvas'); const x=c.getContext('2d');
   let n=0;
   (function f(){
     // All within ONE frame: the display canvas is not persistent across frames
     // yet (documented in runtime/shims/canvas.js), and every corpus game redraws
     // fully each frame anyway. Testing across frames would assert a behaviour
     // this renderer does not currently claim.
     x.fillStyle='#000'; x.fillRect(0,0,c.width,c.height);
     x.fillStyle='red';  x.fillRect(10,10,40,20);
     const src = x.getImageData(0,0,120,120);
     x.putImageData(src, 400, 200);
     if(n===2){
       // The rect is at (10,10)-(50,30). Sample a point INSIDE it, and the
       // corresponding point in the copy at the same offset: the copy is placed
       // at (400,200), so source (20,15) maps to (410,205)... which is OUTSIDE
       // the rect vertically once you account for the rect starting at y=10 with
       // height 20. Sample (20,20) -> (410,210), comfortably inside both.
       const a = x.getImageData(20,20,1,1).data;
       const b = x.getImageData(410,210,1,1).data;
       console.log('R:'+a[0]+','+a[1]+','+a[2]+'|'+b[0]+','+b[1]+','+b[2]);
     }
     if(++n<5) requestAnimationFrame(f);
   })();`,
  (o) => {
    const m = /R:(\S+)/.exec(o);
    if (!m) return { ok: false, detail: 'no result' };
    const [src, copy] = m[1].split('|');
    return { ok: src === copy && src !== '0,0,0',
             detail: `src ${src}, copy ${copy}` };
  });

check('canvas2d: putImageData REFUSES a short buffer (MUST-FAIL control)',
  `const x=document.getElementById('game-canvas').getContext('2d');
   try { x.putImageData({width:64,height:64,data:new Uint8ClampedArray(16)},0,0);
         console.log('R:ACCEPTED'); }
   catch(e){ console.log('R:REFUSED:' + /needs \\d+ bytes/.test(e.message)); }`,
  (o) => {
    // The five-times-burned bug class: trusting claimed dimensions over the real
    // buffer length. Natively that is memory corruption, not garbled pixels.
    const m = /R:(\S+)/.exec(o);
    return { ok: m && m[1] === 'REFUSED:true', detail: m ? m[1] : 'no result' };
  });

check('canvas2d: unimplemented methods THROW BY NAME, never no-op',
  `const x=document.getElementById('game-canvas').getContext('2d');
   // createLinearGradient is genuinely unimplemented. (This check previously used
   // arc(), which has since been implemented — a test asserting "X is missing"
   // has to move when X arrives, or it silently stops testing anything.)
   try { x.createLinearGradient(0,0,1,1); console.log('R:SILENT'); }
   catch(e){ console.log('R:' + (/createLinearGradient is not implemented/.test(e.message)
                                  ? 'NAMED' : 'UNNAMED')); }`,
  (o) => ({ ok: /R:NAMED/.test(o), detail: (/R:(\S+)/.exec(o) || [])[1] }));

check('canvas2d: the path API is implemented (measured as heavily used)',
  `const c=document.getElementById('game-canvas'); const x=c.getContext('2d');
   let n=0;
   (function f(){
     x.fillStyle='#000'; x.fillRect(0,0,c.width,c.height);
     x.beginPath(); x.moveTo(50,50); x.lineTo(150,50); x.lineTo(100,140); x.closePath();
     x.fillStyle='red'; x.fill();
     x.beginPath(); x.arc(300,100,50,0,Math.PI*2); x.fillStyle='lime'; x.fill();
     if(n===2){
       const tri=x.getImageData(100,80,1,1).data, cir=x.getImageData(300,100,1,1).data;
       console.log('R:'+tri[0]+','+tri[1]+','+tri[2]+'|'+cir[0]+','+cir[1]+','+cir[2]);
     }
     if(++n<5) requestAnimationFrame(f);
   })();`,
  (o) => {
    const m = /R:(\S+)/.exec(o);
    if (!m) return { ok: false, detail: 'no result' };
    const [tri, cir] = m[1].split('|');
    return { ok: tri === '255,0,0' && cir === '0,255,0',
             detail: `triangle ${tri}, circle ${cir}` };
  });

/* ----------------------------------------------------------------- events ---- */

check('events: click reaches a document once-listener (audio-unlock pattern)',
  `let n=0;
   document.addEventListener('click', () => { n++; }, {once:true});
   globalThis.__jsglq_dispatchEvent({type:'click',clientX:1,clientY:1,button:0});
   globalThis.__jsglq_dispatchEvent({type:'click',clientX:2,clientY:2,button:0});
   console.log('R:'+n);`,
  (o) => ({ ok: /R:1/.test(o), detail: (/R:(\d+)/.exec(o) || [])[1] }));

check('events: KeyboardEvent.code is a real code, not an SDL key name',
  `let got='';
   window.addEventListener('keydown', e => { got = e.code + '/' + e.key; });
   globalThis.__jsglq_dispatchEvent({type:'keydown', code:'KeyA', key:'a'});
   console.log('R:'+got);`,
  (o) => ({ ok: /R:KeyA\/a/.test(o), detail: (/R:(\S+)/.exec(o) || [])[1] }));

/* ------------------------------------------------------------------ image ---- */

check('image: a corrupt PNG reports an error rather than failing silently',
  `const img = new Image();
   img.onload = () => console.log('R:LOADED');
   img.onerror = () => console.log('R:ERROR');
   img.src = 'data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAABAAAAAQCAYAAAAf8/9h';`,
  (o) => ({ ok: /R:ERROR/.test(o), detail: (/R:(\S+)/.exec(o) || [])[1] }));

/* ----------------------------------------------------------------- worker ---- */

check('worker: short message ending in a number survives a longer predecessor',
  `const w = new Worker('worker.js'); const got=[];
   w.onmessage = (e) => { got.push(e.data.got);
     if(got.length===2) console.log('R:'+JSON.stringify(got)); };
   setTimeout(()=>{ w.postMessage({payload:'${'x'.repeat(60)}'});
                    w.postMessage({payload:7}); }, 40);
   let n=0;(function f(){ if(++n<100) requestAnimationFrame(f); })();`,
  (o) => {
    // WAS RED in the wasm build: QuickJS's JSON tokenizer reads past the given
    // length when a value ends in a number, so the short message inherited the
    // long one's tail. Engine behaviour, so it applies natively too.
    const m = /R:(\S+)/.exec(o);
    return { ok: m && m[1].includes('7'), detail: m ? m[1] : 'no result' };
  }, {
    frames: 140,
    files: { 'worker.js': `self.onmessage = (e) => postMessage({got: e.data.payload});` },
  });

check('worker: runs on a real thread (parallel, not cooperative)',
  `const N=3, workers=[], results=[]; let started=0;
   for(let i=0;i<N;i++){ const w=new Worker('worker.js');
     w.onmessage=(e)=>{ results.push(e.data.ms);
       if(results.length===N){ const wall=Date.now()-started;
         const sum=results.reduce((a,b)=>a+b,0);
         console.log('R:'+(sum/wall).toFixed(2)); } };
     workers.push(w); }
   setTimeout(()=>{ started=Date.now(); for(const w of workers) w.postMessage({n:2000000}); },80);
   let n=0;(function f(){ if(++n<280) requestAnimationFrame(f); })();`,
  (o) => {
    const m = /R:([\d.]+)/.exec(o);
    return { ok: m && parseFloat(m[1]) > 1.8,
             detail: m ? `${m[1]}x speedup` : 'no result' };
  }, {
    frames: 320,
    files: { 'worker.js': `self.onmessage = (e) => {
      const t0 = Date.now(); let s = 0;
      for (let i = 0; i < e.data.n; i++) s += Math.sqrt(i);
      postMessage({ ms: Date.now() - t0, s });
    };` },
  });

/* ------------------------------------------------------------------ audio ---- */

check('audio: latency is frame-scale, not the 1-2s of the queue-and-poll model',
  `const A = globalThis.__jsglq_audio;
   try {
     A.init();
     const s = A.stats();
     console.log('R:' + s.latencyMs.toFixed(1) + ':' + s.underruns);
   } catch (e) {
     // A headless CI runner has no sound card, so opening a device legitimately
     // fails. That must be reported as NO-DEVICE rather than silently passing:
     // a test that quietly succeeds when it could not run is worse than one that
     // fails, because it claims coverage it does not have.
     console.log('R:NO-DEVICE:' + e.message.slice(0, 60));
   }`,
  (o) => {
    if (/R:NO-DEVICE/.test(o)) {
      // Verify the FAILURE is clean and named, which is the part that still
      // matters without hardware: a game on a machine with no audio must get a
      // clear error, not a crash or silence.
      return { ok: /could not open audio device|audio init failed/i.test(o),
               detail: 'no audio device on this machine; declined cleanly (CI)' };
    }
    const m = /R:([\d.]+):(\d+)/.exec(o);
    return { ok: m && parseFloat(m[1]) < 50 && m[2] === '0',
             detail: m ? `${m[1]}ms, ${m[2]} underruns` : 'no result' };
  });

/* --------------------------------------------------------------- lifecycle ---- */

check('shutdown: no leaked JSValues (QuickJS asserts if any survive)',
  `const c=document.getElementById('game-canvas'); const x=c.getContext('2d');
   const img=new Image(); img.src='data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAABAAAAAQCAYAAAAf8/9hAAAAI0lEQVR42mPQ7tr6nxLMMGoAdgMeHJHEikcNoKcBoymRNAwALe50y1hW5DEAAAAASUVORK5CYII=';
   setTimeout(()=>{},5); setInterval(()=>{},1000);
   requestAnimationFrame(()=>{}); requestAnimationFrame(()=>{});
   x.fillStyle='red'; x.fillRect(0,0,10,10);
   console.log('R:done');`,
  (o) => ({ ok: /R:done/.test(o) && !/Assertion/.test(o),
            detail: /Assertion/.test(o) ? 'leaked values at shutdown' : 'clean' }));

check('unhandled rejections are LOUD, not silent',
  `console.log('R:start'); (async () => { throw new Error('boom'); })();`,
  (o) => ({ ok: /UNHANDLED PROMISE REJECTION/.test(o) && /boom/.test(o),
            detail: /UNHANDLED/.test(o) ? 'reported with reason' : 'SILENTLY SWALLOWED' }));

/* ------------------------------------------------------------------- report -- */

console.log(`\n${pass} passed, ${fail} failed`);
if (fail) {
  console.log('\nFailures:');
  for (const f of failures) {
    console.log(`\n--- ${f.name} ---\n${f.detail}\n${f.out}`);
  }
  process.exit(1);
}
