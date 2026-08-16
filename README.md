# jsgamelauncher-quickjs

Run browser-API games with **no browser and no Node**, from a single ~11 MB binary.

QuickJS + SDL2 + OpenGL ES. Games use `canvas`, `WebGL2`, `AudioContext`,
`requestAnimationFrame`, `fetch`, `localStorage` and the Gamepad API exactly as they
would in a page — the same `main.js` runs in a browser and here.

```bash
./jsglq examples/hello-canvas          # play a game directory
./jsglq --fullscreen ./my-game         # fullscreen
node scripts/fuse.mjs ./my-game dist   # ship your game as ONE executable
```

## Why

| | this | jsgamelauncher (Node) | Electron |
|---|---|---|---|
| binary / install | **~11 MB** | Node + ~88 npm packages | ~150 MB+ |
| ships as one file | **yes** | no | with work |
| audio latency | **~12 ms** | 1–2 s (queue-and-poll) | ~20 ms |
| sandbox | **structural** (no libc compiled in) | `node:vm`, escapable in one line | full Node access |

## Performance

**Read this before the table.** These are *my* numbers on *one* machine, and they
are published because the shape of the result is useful, not because the digits are
authoritative. Run `node bench/run.mjs` yourself; hardware, driver and compositor
all move them.

**How it was measured.** One harness file runs the **same scene source** under all
three runtimes — this one, jsgamelauncher on Node, and Chrome driven by Playwright.
Scenes are deterministic (seeded PRNG, fixed virtual timestep), so every runtime
executes the same instruction stream. 120 warmup frames are discarded, 600 are
measured, 3 runs per scene, median reported, CPU governor pinned to `performance`.
"Busy" is time inside the frame callback — the work the game does — measured
separately from frame interval, because conflating them hides both.

**Caveats that matter:**

- **Chrome is a browser, not a game runtime.** It carries compositor, security and
  process-isolation costs that a single-purpose launcher skips. Beating it on 2D is
  not a claim that Chrome is badly built.
- **Different renderers.** This runtime draws Canvas 2D as batched textured quads on
  GL; Node's jsgamelauncher uses Skia on the CPU and uploads a full texture each
  frame. Same API, different engine — some of the gap is that choice, not the JS.
- **The 3D scenes are deliberately hostile.** "three.js heavy" does 2000 per-instance
  matrix composes plus a bone chain and shadows every frame, chosen to find the
  ceiling. Most real games are nowhere near it.
- **Colour counts and frame timings drift** between runs and GPUs. Treat any single
  figure as ±20%, and treat the ratios as the finding.
- **Nothing here is tuned for the benchmark.** No scene-specific fast paths.

Busy ms/frame, p50, median of 3 runs, AMD Ryzen AI 9 HX 370 / Radeon RX 7600, 960×540.
Lower is better; **bold** marks the winner.

| scene | what it stresses | qjs | Node | Chrome |
|---|---|---|---|---|
| sprites (1200 blits) | drawImage, canvas binding | **0.60** | 3.53 | 5.19 |
| canvas2d storm | fillRect/fillText/transforms | **0.56** | 2.28 | 1.22 |
| entities ×5000 | vector math + `Array.sort` | **4.85** | 9.64 | 12.91 |
| entities ×500 | same, corpus scale | **0.41** | 0.79 | 0.63 |
| three.js basic | 200 cubes, 2 lights | 5.17 | 2.04 | **1.44** |
| three.js PBR | textures, 4 lights | 3.88 | 1.92 | **0.68** |
| three.js heavy | 2000 instanced + skinned + shadows | 17.03 | 6.06 | **2.06** |
| scene-graph math | 5000 nodes, no GL | 47.24 | 19.77 | **2.79** |
| GC churn | 30k short-lived objects/frame | 7.39 | 0.22 | **0.09** |
| typed arrays | 4 MB per-element loop | 53.16 | 2.38 | **2.22** |

**It wins on 2D and loses on heavy 3D math.** The split is not subtle, and it comes
from one cause: QuickJS has no JIT, so work done *in C* is fast and work done *in a
JS loop* is not.

Isolated with plain-JS microbenchmarks — no GL, no bindings, no library:

| microbench | qjs | Node (V8 JIT) | ratio |
|---|---|---|---|
| `Float32Array.set`, 4 MB (bulk) | 0.069 ms | 0.069 ms | **1.0×** |
| Float32Array, 4 MB element-by-element | 54.1 ms | 0.37 ms | 145× |
| 30k short-lived objects | 6.04 ms | 0.064 ms | 94× |
| matrix compose ×5000 | 3.00 ms | 0.134 ms | 22× |

A bulk copy is *identical* in both runtimes. The same bytes touched one element at a
time are 145× apart. That is the whole story: 2D games spend their time inside C
(`fillRect` is one call), while heavy 3D spends it in JS matrix loops.

**So:** ship 2D and light 3D here; point heavy three.js at
[jsgamelauncher](https://github.com/monteslu/jsgamelauncher), which has V8.

This is a real limit, not a tuning problem. No JIT means no way to close a 145× gap
on per-element JS loops by optimizing bindings — the bindings are already as thin as
they get. If your game's hot loop is JS matrix math over thousands of objects, use a
JIT runtime. If it draws sprites and text, this is the faster and much smaller
option.

## Platform support

Six targets, the same set [native-gles](https://github.com/monteslu/native-gles)
builds and ships:

| platform | GLES source | status |
|---|---|---|
| linux-x64, linux-arm64 | system EGL/GLES | built + smoke-tested in CI |
| macos-arm64, macos-x64 | ANGLE (Metal) | built in CI; ANGLE ships beside the binary |
| windows-x64, windows-arm64 | ANGLE (D3D11) | built in CI; ANGLE ships beside the binary |

Platform-specific code is confined to `src/platform.c` — executable path, path
splitting, mkdir, realpath, temp dir. Everything else is portable C/C++.

macOS and Windows have no system GLES, so ANGLE is fetched with native-gles's own
`download-angle` script rather than a second, divergent copy. CI runners have no
display, so those two run a load-and-report check while Linux runs the full
pixel-verifying smoke test.

## Building

Needs SDL2, EGL and GLES dev packages, plus
[native-gles](https://github.com/monteslu/native-gles) and
[webaudio-node](https://github.com/monteslu/webaudio-node) checked out beside this
repo — both are compiled from source, unmodified.

```bash
node scripts/fetch-deps.mjs      # quickjs-ng, at a verified pinned commit
cmake -S . -B build-cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build-cmake -j8
./build/jsglq examples/hello-canvas
```

## Shipping a game

```bash
node scripts/fuse.mjs ./my-game my-game-linux
./my-game-linux                  # takes no arguments; it IS the game
```

The runtime layer travels inside the binary, so it runs from anywhere. On macOS,
**sign after fusing** — appending invalidates an earlier signature.

Commercial games are fine. Every embedded component permits commercial use and
redistribution; run `./jsglq --licenses` or read
[THIRD-PARTY.md](THIRD-PARTY.md). Include that file with your game and you have met
every attribution requirement.

## What works

| area | state |
|---|---|
| WebGL2 | 247 entry points; three.js r183 renders (PBR, instancing, skinning, shadows) |
| Canvas 2D | rects, text, images, transforms, and the full path API |
| Input | keyboard (real `KeyboardEvent.code`), mouse, pointer, wheel, gamepad |
| Audio | mp3/wav/flac/ogg decode, Web Audio graph, `<audio>`, **~12 ms latency** |
| Workers | real OS threads, **2.5–3.4× measured speedup** |
| Storage | `localStorage` persists to disk; `sessionStorage` separate |
| Networking | `fetch` and `XHR`, rooted at the game directory |

**Not supported:** WebAssembly (deferred — WAMR trails V8 at wasm by more than
QuickJS trails V8 at JS, so wasm games get a better experience on jsgamelauncher),
video playback, gradients, and patterns. Unimplemented Canvas methods **throw with
their own name** rather than silently doing nothing.

## Corpus parity

`node test/parity/sweep.mjs` runs 14 real games and reports what actually
**rendered** — colour counts prove it drew, distinct frame CRCs prove it animated —
rather than whether the process exited zero.

Currently **6 of 11 non-wasm games render**, and all 3 wasm titles decline cleanly
with a named error. Still failing: `simple-phaser` (boots and issues 360 draw calls
but produces no visible output), `space` / `space3d` (~570 MB of startup audio
decode), and two that pass at 960×540 but regress at 640×480.

## Testing

```bash
node bench/selftest.mjs          # harness self-test, incl. 4 must-fail controls
node test/regression/run.mjs     # 17 checks, incl. 2 must-fail controls
node test/smoke.mjs              # requires real drawn pixels, not exit code 0
node test/parity/sweep.mjs       # all 14 real games
node bench/run.mjs --runtime=qjs --scene=all --runs=3
node bench/report.mjs            # the comparison table above
```

The bench harness was built and self-tested against Chrome and Node **before this
runtime existed**, so its first measurement was never taken with an unvalidated rig.
Its must-fail controls (a 1px-shifted image, a flipped frame, an injected 5 ms
stall) run in CI: if one ever passes, the harness is broken and its numbers are void.

## License

MIT. See [LICENSE](LICENSE) and [THIRD-PARTY.md](THIRD-PARTY.md).
