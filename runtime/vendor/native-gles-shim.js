/*
 * `native-gles` as webgl-node expects it.
 *
 * The host installs the identical function set (247 GL entry points plus context
 * management) on globalThis.gl, generated from native-gles's own module.cpp. This
 * module just re-exports it as a default export so webgl-node's
 * `import gl from 'native-gles'` resolves without modification.
 *
 * webgl-node is vendored VERBATIM: it is pure ESM with zero Node imports, so there
 * is nothing to port. If it ever needs a patch, that is a signal the host binding
 * diverged from native-gles rather than a reason to fork it.
 */
const gl = globalThis.gl;
if (!gl) {
  throw new Error('native-gles shim: host GL bindings missing (jsglq_bind_gl_object not run)');
}
export default gl;
