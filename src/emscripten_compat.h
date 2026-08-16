/*
 * Emscripten compatibility shim, so webaudio-node's C++ DSP engine compiles
 * NATIVELY without editing it.
 *
 * The engine (audio_graph_simple.cpp and friends, ~3000 lines: 16 node types plus
 * the mp3/wav/flac/vorbis/aac decoders) is ordinary portable C++. Its only ties to
 * Emscripten are two headers and one macro:
 *
 *   <emscripten.h>       for EMSCRIPTEN_KEEPALIVE (an export annotation)
 *   <wasm_simd128.h>     for the WASM SIMD intrinsics
 *
 * Compiled natively, EMSCRIPTEN_KEEPALIVE just needs to mean "keep this symbol and
 * give it C linkage", which is what a visibility attribute does. That is the whole
 * gap between "runs in V8's wasm engine" and "runs as native code in this host".
 *
 * NOTE ON SIMD: the engine guards its SIMD paths behind __wasm_simd128__, which is
 * not defined natively, so the scalar fallbacks compile instead. Those are the same
 * algorithms, and the native compiler auto-vectorizes much of it anyway. Getting the
 * SSE/NEON paths in is a later optimization, not a correctness matter.
 */
#ifndef JSGLQ_EMSCRIPTEN_COMPAT_H
#define JSGLQ_EMSCRIPTEN_COMPAT_H

#ifdef __EMSCRIPTEN__
#error "emscripten_compat.h is for NATIVE builds only"
#endif

/*
 * Emscripten's headers transitively pull in <cstdint>, and the engine relies on
 * that without including it. Providing it here keeps the upstream sources
 * unedited, which is the whole point of this shim.
 */
#ifdef __cplusplus
#include <cstdint>
#include <cstddef>
#include <string>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#else
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#endif

/* Keep the symbol and give it C linkage, exactly as the wasm build does. */
#ifdef __cplusplus
#define EMSCRIPTEN_KEEPALIVE extern "C" __attribute__((used, visibility("default")))
#else
#define EMSCRIPTEN_KEEPALIVE __attribute__((used, visibility("default")))
#endif

/* The engine does not call these, but they appear in emscripten.h and some
   translation units reference them defensively. */
#define EM_ASM(...)            ((void)0)
#define EM_ASM_INT(...)        (0)
#define EM_ASM_DOUBLE(...)     (0.0)
#define EMSCRIPTEN_BINDINGS(x) static void emscripten_bindings_##x(void)

#endif /* JSGLQ_EMSCRIPTEN_COMPAT_H */
