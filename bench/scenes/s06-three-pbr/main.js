/**
 * s06-three-pbr — the reference-comparable tier.
 *
 * Shaped after the wasmcart-jsgame threejs example (PBR materials, procedural
 * textures, several lights), which measured ~1800 fps uncapped under QuickJS-in-WASM.
 * That gives this scene an external datapoint to sanity-check the native build
 * against: native QuickJS should not be slower than QuickJS inside V8's wasm engine.
 *
 * Textures are generated procedurally rather than decoded from PNG so the scene is
 * byte-deterministic across runtimes and has no asset-loading dependency. The image
 * decode path is s04's job.
 */
import { autorun } from '../../harness.js';
import { makeThreeScene } from '../lib/threescene.js';

const COUNT = 120;

function proceduralTexture(THREE, size = 64) {
  const data = new Uint8Array(size * size * 4);
  for (let y = 0; y < size; y++) {
    for (let x = 0; x < size; x++) {
      const i = (y * size + x) * 4;
      const checker = ((x >> 3) + (y >> 3)) & 1;
      const n = ((x * 31 + y * 17) % 53) * 2;
      data[i] = checker ? 200 - n : 60 + n;
      data[i + 1] = checker ? 170 - n : 90 + n;
      data[i + 2] = checker ? 120 + n : 140 - n;
      data[i + 3] = 255;
    }
  }
  const tex = new THREE.DataTexture(data, size, size, THREE.RGBAFormat);
  tex.wrapS = tex.wrapT = THREE.RepeatWrapping;
  tex.needsUpdate = true;
  return tex;
}

export const scene = makeThreeScene('s06-three-pbr', ({ THREE, scene, rng }) => {
  scene.add(new THREE.AmbientLight(0x30363d, 1.5));
  const key = new THREE.DirectionalLight(0xfff0dd, 2.4);
  key.position.set(18, 28, 12);
  scene.add(key);
  const rim = new THREE.PointLight(0x66aaff, 220, 140);
  rim.position.set(-20, 10, -18);
  scene.add(rim);
  const fill = new THREE.PointLight(0xffaa66, 160, 120);
  fill.position.set(22, -8, 16);
  scene.add(fill);

  const map = proceduralTexture(THREE);
  const geos = [
    new THREE.BoxGeometry(2.4, 2.4, 2.4),
    new THREE.SphereGeometry(1.5, 24, 16),
    new THREE.TorusGeometry(1.4, 0.5, 12, 24),
  ];

  const ground = new THREE.Mesh(
    new THREE.PlaneGeometry(120, 120),
    new THREE.MeshStandardMaterial({ color: 0x2a3038, roughness: 0.9, metalness: 0.0 }),
  );
  ground.rotation.x = -Math.PI / 2;
  ground.position.y = -14;
  scene.add(ground);

  const meshes = [];
  for (let i = 0; i < COUNT; i++) {
    const mat = new THREE.MeshStandardMaterial({
      map,
      color: new THREE.Color().setHSL((i * 37 % 360) / 360, 0.5, 0.6),
      roughness: 0.25 + rng() * 0.6,
      metalness: rng() * 0.85,
    });
    const m = new THREE.Mesh(geos[i % geos.length], mat);
    m.position.set((rng() - 0.5) * 56, (rng() - 0.5) * 22, (rng() - 0.5) * 56);
    m.userData.spin = 0.3 + rng() * 1.2;
    m.userData.bob = rng() * Math.PI * 2;
    scene.add(m);
    meshes.push(m);
  }

  return {
    meshes,
    update(t, frame, dt, time) {
      for (let i = 0; i < t.meshes.length; i++) {
        const m = t.meshes[i];
        m.rotation.x = time * m.userData.spin;
        m.rotation.z = time * m.userData.spin * 0.6;
        m.position.y += Math.sin(time * 2 + m.userData.bob) * 0.02;
      }
    },
  };
});

autorun(scene);
