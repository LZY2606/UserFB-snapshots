import assert from 'node:assert/strict';
import * as THREE from 'three';
import { loadControls } from './build.mjs';
const Controls = await loadControls();
Controls.install({THREE});
const camera = new THREE.PerspectiveCamera(60, 1.5, .1, 1000);
camera.position.set(8, 5, 11);
const controls = new Controls(camera);
const box = new THREE.Box3(new THREE.Vector3(-2,-1,-3),new THREE.Vector3(2,1,3));
await controls.fitToBox(box, false);
controls.update(1/60); camera.updateMatrixWorld();
let maxXY = 0;
for (const x of [-2,2]) for (const y of [-1,1]) for (const z of [-3,3]) {
  const p = new THREE.Vector3(x,y,z).project(camera);
  assert.ok(Math.abs(p.x)<=1.00001 && Math.abs(p.y)<=1.00001 && Math.abs(p.z)<=1);
  maxXY = Math.max(maxXY,Math.abs(p.x),Math.abs(p.y));
}
console.log(`BASELINE axis-fit corners=8 maxNdc=${maxXY.toFixed(6)} pointsAPI=${typeof controls.fitToPoints}`);
controls.dispose();
