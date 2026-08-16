/**
 * Shared three.js renderer scaffold for s05/s06/s07.
 *
 * The three scenes differ only in what they put in the scene graph, so the renderer
 * setup, camera rig, and canary live here. Divergent setup would make the tiers
 * incomparable, which is the whole point of having three of them.
 *
 * Note the renderer is constructed with an explicit `context`. three.js accepts a
 * pre-created WebGL2 context, which is the path a non-browser host must support
 * (webgl-node's mock canvas satisfies the property probing three does at init).
 */
import { drawCanaryGl } from '../../harness.js';
import { getGl } from './scaffold.js';
import * as THREE from './vendor/three.js';

export function makeThreeScene(name, build) {
  return {
    name,

    setup({ width, height, rng }) {
      const req = getGl(width, height, {
        antialias: false, alpha: false, depth: true, stencil: false,
        preserveDrawingBuffer: true,   // capture reads the drawn frame, not a cleared one
      });
      const { canvas, gl } = req;

      /*
       * Use the ACTUAL drawing-buffer size, not the requested one.
       *
       * rungame builds its GL display context and gameFBO at a hardcoded 640x480
       * (launcher.js DEFAULT_GAME_WIDTH/HEIGHT) regardless of what the game sets
       * canvas.width to. Rendering and reading back at the requested size then
       * samples outside the real surface and returns undefined pixels — which is
       * how three corners of the orientation canary came back black while the
       * frame rate looked perfectly healthy.
       *
       * Every runtime reports its truth in gl.drawingBufferWidth/Height, so that is
       * what the scene, the viewport, and the capture all agree on.
       */
      const bw = gl.drawingBufferWidth || width;
      const bh = gl.drawingBufferHeight || height;
      if (bw !== width || bh !== height) {
        console.log(`[bench] drawing buffer is ${bw}x${bh}, requested ${width}x${height}` +
                    ` — scene will use the real size`);
      }
      width = bw; height = bh;

      const renderer = new THREE.WebGLRenderer({ canvas, context: gl, antialias: false });
      renderer.setPixelRatio(1);
      renderer.setSize(width, height, false);
      renderer.setClearColor(0x0d1014, 1);

      const scene3 = new THREE.Scene();
      // Aspect from the real buffer, so geometry lands in the same place in every
      // runtime even when their surfaces differ in size.
      const camera = new THREE.PerspectiveCamera(60, width / height, 0.1, 500);
      camera.position.set(0, 12, 42);
      camera.lookAt(0, 0, 0);

      const built = build({ THREE, scene: scene3, camera, renderer, rng, width, height });

      return { canvas, gl, renderer, scene3, camera, width, height, ...built };
    },

    step(t, frame, dt) {
      const time = frame / 60;
      if (t.update) t.update(t, frame, dt, time);

      t.camera.position.x = Math.sin(time * 0.3) * 42;
      t.camera.position.z = Math.cos(time * 0.3) * 42;
      t.camera.lookAt(0, 0, 0);

      t.renderer.render(t.scene3, t.camera);

      // three.js leaves its own state bound; reset so the canary's scissor/clear
      // work does not fight the renderer's cached state on the next frame.
      t.renderer.resetState();
      drawCanaryGl(t.gl, t.width, t.height);
    },

    teardown(t) {
      try { t.renderer.dispose(); } catch (_) {}
    },
  };
}
