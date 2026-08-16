/**
 * s07-three-heavy — THE GO/NO-GO SCENE.
 *
 * This is the scene the whole project is gated on (decision gate D1). It is built to
 * be the most hostile realistic three.js workload a game might ship:
 *
 *   - 2000 instanced meshes with per-instance matrix updates every frame. Instancing
 *     moves the draw-call cost to the GPU but leaves the MATRIX MATH in JS, which is
 *     exactly the shape that punishes an interpreter.
 *   - A skinned mesh with an animated bone chain (per-frame bone matrix composition).
 *   - A shadow-casting directional light (a second full scene traversal + depth pass).
 *   - Several hundred individually-transformed non-instanced meshes so the per-object
 *     render path is exercised too, not just the instanced fast path.
 *
 * Budget (from the plan): busy p95 <= 12 ms on the desktop reference box at 1080p,
 * and <= 12 ms on the weak ARM box at 720p. Miss it and the fallbacks are Hermes
 * (if s08 says the interpreter is the wall) or Bun (if nothing closes the gap).
 *
 * Deliberately NOT using KTX2/DRACO: those need WASM transcoders, and WASM is
 * out of scope for v1.
 */
import { autorun } from '../../harness.js';
import { makeThreeScene } from '../lib/threescene.js';

const INSTANCES = 2000;
const LOOSE_MESHES = 300;
const BONES = 24;

export const scene = makeThreeScene('s07-three-heavy', ({ THREE, scene, camera, renderer, rng }) => {
  renderer.shadowMap.enabled = true;
  renderer.shadowMap.type = THREE.PCFShadowMap;

  scene.add(new THREE.AmbientLight(0x353b44, 1.4));
  const sun = new THREE.DirectionalLight(0xffffff, 2.6);
  sun.position.set(30, 50, 20);
  sun.castShadow = true;
  sun.shadow.mapSize.set(1024, 1024);
  sun.shadow.camera.left = -60; sun.shadow.camera.right = 60;
  sun.shadow.camera.top = 60; sun.shadow.camera.bottom = -60;
  scene.add(sun);

  const ground = new THREE.Mesh(
    new THREE.PlaneGeometry(160, 160),
    new THREE.MeshStandardMaterial({ color: 0x262c34, roughness: 0.95 }),
  );
  ground.rotation.x = -Math.PI / 2;
  ground.position.y = -16;
  ground.receiveShadow = true;
  scene.add(ground);

  // --- instanced field: the per-instance matrix math is the point ---------------
  const instGeo = new THREE.BoxGeometry(1.2, 1.2, 1.2);
  const instMat = new THREE.MeshStandardMaterial({ color: 0x6fa8dc, roughness: 0.4, metalness: 0.3 });
  const inst = new THREE.InstancedMesh(instGeo, instMat, INSTANCES);
  inst.castShadow = true;
  inst.instanceMatrix.setUsage(THREE.DynamicDrawUsage);
  const seeds = new Float32Array(INSTANCES * 4);
  for (let i = 0; i < INSTANCES; i++) {
    seeds[i * 4 + 0] = (rng() - 0.5) * 110;
    seeds[i * 4 + 1] = (rng() - 0.5) * 40;
    seeds[i * 4 + 2] = (rng() - 0.5) * 110;
    seeds[i * 4 + 3] = rng() * Math.PI * 2;
  }
  scene.add(inst);

  // --- skinned mesh: bone matrix composition every frame ------------------------
  const skinGeo = new THREE.CylinderGeometry(1.2, 1.2, 24, 12, BONES - 1);
  const pos = skinGeo.attributes.position;
  const skinIndices = [], skinWeights = [];
  for (let i = 0; i < pos.count; i++) {
    const y = pos.getY(i) + 12;
    const seg = (y / 24) * (BONES - 1);
    const idx = Math.floor(seg);
    const w = seg - idx;
    skinIndices.push(idx, Math.min(idx + 1, BONES - 1), 0, 0);
    skinWeights.push(1 - w, w, 0, 0);
  }
  skinGeo.setAttribute('skinIndex', new THREE.Uint16BufferAttribute(skinIndices, 4));
  skinGeo.setAttribute('skinWeight', new THREE.Float32BufferAttribute(skinWeights, 4));

  const bones = [];
  let prev = null;
  for (let i = 0; i < BONES; i++) {
    const b = new THREE.Bone();
    b.position.y = i === 0 ? -12 : 24 / (BONES - 1);
    if (prev) prev.add(b); else scene.add(b);
    bones.push(b);
    prev = b;
  }
  const skinned = new THREE.SkinnedMesh(
    skinGeo,
    new THREE.MeshStandardMaterial({ color: 0xe0a458, roughness: 0.5, metalness: 0.2 }),
  );
  skinned.castShadow = true;
  skinned.add(bones[0]);
  skinned.bind(new THREE.Skeleton(bones));
  skinned.position.set(0, 0, 0);
  scene.add(skinned);

  // --- loose meshes: the per-object render path ---------------------------------
  const looseGeo = new THREE.IcosahedronGeometry(1.0, 0);
  const loose = [];
  for (let i = 0; i < LOOSE_MESHES; i++) {
    const m = new THREE.Mesh(looseGeo, new THREE.MeshStandardMaterial({
      color: new THREE.Color().setHSL((i * 29 % 360) / 360, 0.6, 0.55),
      roughness: 0.6,
    }));
    m.position.set((rng() - 0.5) * 100, (rng() - 0.5) * 34, (rng() - 0.5) * 100);
    m.castShadow = true;
    m.userData.spin = 0.5 + rng();
    scene.add(m);
    loose.push(m);
  }

  const dummy = new THREE.Object3D();

  return {
    inst, seeds, bones, loose, dummy,
    update(t, frame, dt, time) {
      // 2000 x (compose TRS -> Matrix4 -> write into the instance buffer).
      const { inst, seeds, dummy } = t;
      for (let i = 0; i < INSTANCES; i++) {
        const o = i * 4;
        const ph = seeds[o + 3];
        dummy.position.set(
          seeds[o + 0],
          seeds[o + 1] + Math.sin(time * 1.5 + ph) * 3,
          seeds[o + 2],
        );
        dummy.rotation.set(time * 0.5 + ph, time * 0.35 + ph, 0);
        const s = 0.8 + Math.sin(time + ph) * 0.2;
        dummy.scale.set(s, s, s);
        dummy.updateMatrix();
        inst.setMatrixAt(i, dummy.matrix);
      }
      inst.instanceMatrix.needsUpdate = true;

      // Bone chain: each bone's world matrix depends on its parent, so this is a
      // serial dependency chain the interpreter cannot pipeline away.
      for (let i = 0; i < t.bones.length; i++) {
        t.bones[i].rotation.z = Math.sin(time * 2 + i * 0.35) * 0.14;
        t.bones[i].rotation.x = Math.cos(time * 1.3 + i * 0.22) * 0.10;
      }

      for (let i = 0; i < t.loose.length; i++) {
        const m = t.loose[i];
        m.rotation.y = time * m.userData.spin;
        m.rotation.x = time * m.userData.spin * 0.5;
      }
    },
  };
});

autorun(scene);
