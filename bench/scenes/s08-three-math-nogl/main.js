/**
 * s08-three-math-nogl — interpreter cost isolated from binding cost.
 *
 * This is the diagnostic scene. It runs three.js scene-graph math (Object3D
 * transforms, Matrix4 composition, Quaternion slerp, frustum culling) on 5000 nodes
 * with NO renderer and NO GL calls at all.
 *
 * Why it matters: if s07 (heavy 3D) misses its budget, this scene says whether the
 * cost is JS execution or the GL binding boundary. Those have opposite fixes, and
 * guessing between them is how weeks get burned. A high qjs/node ratio here means
 * the interpreter is the wall; a low ratio here plus a high one in s07 means the
 * marshaling layer is.
 *
 * Draws a tiny 2D summary so the frame is never blank (blank frames are FAILs) and
 * so the orientation canary still applies.
 */
import { autorun, drawCanaryGl } from '../../harness.js';
import { getGl } from '../lib/scaffold.js';
import * as THREE from '../lib/vendor/three.js';

const NODES = 5000;

export const scene = {
  name: 's08-three-math-nogl',

  setup({ width, height, rng }) {
    // GL rather than 2D: this scene measures scene-graph MATH, and using the GL
    // path keeps it runnable in every runtime (Canvas 2D is a later phase in the
    // QuickJS host). The draw here is a single clear plus the canary, which is
    // negligible against 5000 nodes of matrix work.
    const { canvas, gl } = getGl(width, height, { preserveDrawingBuffer: true });
    const bw = gl.drawingBufferWidth || width;
    const bh = gl.drawingBufferHeight || height;
    width = bw; height = bh;

    const root = new THREE.Object3D();
    const nodes = [];
    // A real hierarchy, not a flat list: updateMatrixWorld's cost is dominated by
    // parent-chain traversal, and a flat list would measure the wrong thing.
    let parent = root;
    for (let i = 0; i < NODES; i++) {
      const o = new THREE.Object3D();
      o.position.set((rng() - 0.5) * 200, (rng() - 0.5) * 200, (rng() - 0.5) * 200);
      o.userData.spin = new THREE.Quaternion().setFromAxisAngle(
        new THREE.Vector3(rng(), rng(), rng()).normalize(), rng() * Math.PI);
      o.userData.rest = new THREE.Quaternion();
      parent.add(o);
      nodes.push(o);
      // Re-root every 8 nodes: depth ~8 chains, similar to a real scene graph.
      parent = (i % 8 === 7) ? root : o;
    }

    const camera = new THREE.PerspectiveCamera(60, width / height, 0.1, 1000);
    camera.position.set(0, 0, 260);
    const frustum = new THREE.Frustum();
    const projScreen = new THREE.Matrix4();
    const tmpVec = new THREE.Vector3();

    return { canvas, gl, root, nodes, camera, frustum, projScreen, tmpVec, width, height };
  },

  step(t, frame) {
    const { gl, root, nodes, camera, frustum, projScreen, tmpVec, width, height } = t;
    const time = frame / 60;

    // Quaternion slerp + position wobble per node: the per-frame math a game does.
    for (let i = 0; i < nodes.length; i++) {
      const o = nodes[i];
      const a = (Math.sin(time + i * 0.01) + 1) * 0.5;
      o.quaternion.slerpQuaternions(o.userData.rest, o.userData.spin, a);
      o.position.y += Math.sin(time * 2 + i) * 0.01;
      o.updateMatrix();
    }

    // The real cost centre: world-matrix propagation over the whole hierarchy.
    root.updateMatrixWorld(true);

    // Frustum culling: matrix multiply + 6 plane tests per node.
    camera.position.x = Math.sin(time * 0.2) * 60;
    camera.lookAt(0, 0, 0);
    camera.updateMatrixWorld(true);
    camera.updateProjectionMatrix();
    projScreen.multiplyMatrices(camera.projectionMatrix, camera.matrixWorldInverse);
    frustum.setFromProjectionMatrix(projScreen);

    let visible = 0;
    for (let i = 0; i < nodes.length; i++) {
      tmpVec.setFromMatrixPosition(nodes[i].matrixWorld);
      if (frustum.containsPoint(tmpVec)) visible++;
    }
    t.visible = visible;

    // Minimal draw so the frame is real (a blank frame is a FAIL) and the canary
    // still applies. A clear plus a scissored bar is far cheaper than the math
    // above, which is the point.
    gl.viewport(0, 0, width, height);
    gl.clearColor(0.05, 0.063, 0.078, 1);
    gl.clear(gl.COLOR_BUFFER_BIT);
    gl.enable(gl.SCISSOR_TEST);
    const barW = Math.max(1, Math.round((visible / nodes.length) * (width - 40)));
    gl.scissor(20, (height / 2 - 10) | 0, barW, 20);
    gl.clearColor(0.31, 0.69, 0.78, 1);
    gl.clear(gl.COLOR_BUFFER_BIT);
    gl.disable(gl.SCISSOR_TEST);
    drawCanaryGl(gl, width, height);
  },
};

autorun(scene);
