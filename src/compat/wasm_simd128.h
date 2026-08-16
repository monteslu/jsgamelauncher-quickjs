/*
 * Intercepts #include <wasm_simd128.h> for native builds.
 *
 * Deliberately EMPTY. webaudio-node guards every SIMD use behind
 * __wasm_simd128__, which a native compiler never defines, so the scalar paths
 * are selected and nothing in this header is referenced. Providing fake
 * intrinsics here would be worse than providing none: they would compile and
 * silently compute the wrong samples.
 */
#ifndef JSGLQ_FAKE_WASM_SIMD128_H
#define JSGLQ_FAKE_WASM_SIMD128_H
#endif
