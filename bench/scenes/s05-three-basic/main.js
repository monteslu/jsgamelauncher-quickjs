/**
 * s05-three-basic — the three.js floor.
 *
 * 200 unlit-ish cubes, 2 lights, no textures, no shadows. If this scene misses
 * budget, three.js is off the table entirely and the answer is not "optimize the
 * scene" but "change engine". It exists to make that a one-line verdict.
 */
import { autorun } from '../../harness.js';
import { makeThreeScene } from '../lib/threescene.js';

const COUNT = 200;

export const scene = makeThreeScene('s05-three-basic', ({ THREE, scene, rng }) => {
  scene.add(new THREE.AmbientLight(0x404850, 2.0));
  const dir = new THREE.DirectionalLight(0xffffff, 2.2);
  dir.position.set(20, 30, 15);
  scene.add(dir);

  const geo = new THREE.BoxGeometry(2, 2, 2);
  const cubes = [];
  for (let i = 0; i < COUNT; i++) {
    const mat = new THREE.MeshLambertMaterial({
      color: new THREE.Color().setHSL((i * 47 % 360) / 360, 0.55, 0.55),
    });
    const m = new THREE.Mesh(geo, mat);
    m.position.set((rng() - 0.5) * 60, (rng() - 0.5) * 24, (rng() - 0.5) * 60);
    m.userData.spin = 0.4 + rng();
    scene.add(m);
    cubes.push(m);
  }

  return {
    cubes,
    update(t, frame, dt, time) {
      for (let i = 0; i < t.cubes.length; i++) {
        const c = t.cubes[i];
        c.rotation.x = time * c.userData.spin;
        c.rotation.y = time * c.userData.spin * 0.7;
      }
    },
  };
});

autorun(scene);
