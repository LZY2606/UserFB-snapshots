import assert from 'node:assert/strict';
import { PlaneGeometry, Mesh, MeshBasicMaterial, DoubleSide, Raycaster, Vector3, Box3 } from 'three';
import { MeshBVH } from '../src/core/MeshBVH.js';
for (const indirect of [false,true]) {
  const g = new PlaneGeometry(4,2,4,2);
  g.clearGroups(); g.addGroup(0,24,0); g.addGroup(24,24,1);
  const bvh = new MeshBVH(g,{indirect,targetLeafSize:1});
  const p = g.attributes.position;
  const changed = 7;
  p.setZ(changed,2); p.needsUpdate = true;
  bvh.refit();
  assert.equal(bvh.getBoundingBox(new Box3()).max.z,2);
  const ray = new Raycaster(new Vector3(0.15,0.2,10),new Vector3(0,0,-1));
  const mesh = new Mesh(g,new MeshBasicMaterial({side:DoubleSide}));
  mesh.updateMatrixWorld(true);
  const expected = ray.intersectObject(mesh)[0];
  const hit = bvh.raycastFirst(ray.ray,DoubleSide);
  assert.ok(expected && hit);
  assert.ok(Math.abs(hit.distance-expected.distance)<1e-7);
  assert.equal(hit.faceIndex,expected.faceIndex);
  assert.ok(hit.uv.distanceTo(expected.uv)<1e-7);
  console.log(`existing full refit PASS indirect=${indirect} triangles=${g.index.count/3}`);
}
