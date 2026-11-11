// Headless regression test for Scene#cloneNode with skinned meshes.
//
// Run with:
//   sh baseline/run-node.sh test/clone-skeleton.js
//
// Covers:
//  - Two meshes sharing one skeleton with different joints palettes
//  - Distinct skeletons that happen to have the same name
//  - In-tree and out-of-tree relativeRootNode / joint node references
//  - Cloning the same subtree twice (independent copies)
//  - Non-identity bind pose (inverse bind matrices must be preserved)
//  - Valid / invalid joint bounding boxes
//  - Plain non-skinned node cloning still works

import assert from 'node:assert/strict';

import Scene from '../src/Scene';
import Node from '../src/Node';
import Mesh from '../src/Mesh';
import Skeleton from '../src/Skeleton';
import Joint from '../src/Joint';
import Material from '../src/Material';
import Geometry from '../src/Geometry';
import BoundingBox from '../src/math/BoundingBox';

function near(actual, expected, message) {
    assert.ok(
        Math.abs(actual - expected) < 1e-4,
        (message || '') + ' expected ' + expected + ', got ' + actual
    );
}

function findByName(root, name) {
    var found = null;
    root.traverse(function (node) {
        if (node.name === name) {
            found = node;
        }
    });
    assert.ok(found, 'node "' + name + '" should exist');
    return found;
}

// ---------------------------------------------------------------------------
// Build the source scene
// ---------------------------------------------------------------------------

// Nodes outside of the cloned subtree. References to them must be kept as is.
var externalRoot = new Node({name: 'externalRoot'});
var outsideBone = new Node({name: 'outsideBone'});
externalRoot.add(outsideBone);
externalRoot.update();

var actor = new Node({name: 'actor'});

// Meshes come BEFORE the bones and the bones live in a sibling branch,
// so cloneNode can not rely on traversal order.
var body = new Mesh({name: 'body'});
var coat = new Mesh({name: 'coat'});
var extra = new Mesh({name: 'extra'});
var extra2 = new Mesh({name: 'extra2'});
var nullRootMesh = new Mesh({name: 'nullRootMesh'});
var statue = new Mesh({name: 'statue'});
statue.geometry = new Geometry();
statue.material = new Material();
var prop = new Node({name: 'prop'});
var propChild = new Node({name: 'propChild'});
prop.add(propChild);

var rigBranch = new Node({name: 'rigBranch'});
var hip = new Node({name: 'hip'});
var limb = new Node({name: 'limb'});
hip.add(limb);
rigBranch.add(hip);

actor.add(body);
actor.add(coat);
actor.add(extra);
actor.add(extra2);
actor.add(nullRootMesh);
actor.add(statue);
actor.add(prop);
actor.add(rigBranch);

// Non-identity bind pose.
hip.position.x = 1;
hip.position.y = 2;
hip.position.z = 3;
limb.position.y = 1; // world (1, 3, 3)

// rigA: shared by body and coat, relativeRootNode inside the subtree.
var rigA = new Skeleton({name: 'rig', relativeRootNode: actor});
rigA.joints.push(new Joint({name: 'hip', index: 0, node: hip}));
rigA.joints.push(new Joint({name: 'limb', index: 1, node: limb}));

// rigB: same name as rigA but a different skeleton. Joint node and
// relativeRootNode are both outside of the cloned subtree.
var rigB = new Skeleton({name: 'rig', relativeRootNode: externalRoot});
rigB.joints.push(new Joint({name: 'strap', index: 0, node: outsideBone}));

// rigC: null relativeRootNode, shares the hip node with rigA.
var rigC = new Skeleton({name: 'nullRig', relativeRootNode: null});
rigC.joints.push(new Joint({name: 'hip2', index: 0, node: hip}));

actor.update();
rigA.updateJointMatrices();
rigB.updateJointMatrices();
rigC.updateJointMatrices();

// Joint bounding boxes: joint 0 valid, joint 1 invalid.
var jointBBox0 = new BoundingBox();
jointBBox0.min.set(-1, -1, -1);
jointBBox0.max.set(1, 1, 1);
jointBBox0.__updated = true;
var jointBBox1 = new BoundingBox();
jointBBox1.__updated = false;
rigA._jointsBoundingBoxes = [jointBBox0, jointBBox1];
rigA.boundingBox = new BoundingBox();

// Meshes share skeletons, each mesh keeps its own joints palette.
body.skeleton = rigA;
body.joints = [0, 1];
coat.skeleton = rigA;
coat.joints = [1, 0];
extra.skeleton = rigB;
extra.joints = [0];
extra2.skeleton = rigB;
extra2.joints = [0];
nullRootMesh.skeleton = rigC;
nullRootMesh.joints = [0];

// Move away from the bind pose after binding: current pose x = 4 while
// bind pose x = 1, so the resting skin translation is 3 on x.
hip.position.x = 4;
actor.update();
rigA.update();
rigB.update();
rigC.update();

near(rigA._skinMatricesArray[12], 3, 'source skin translation x');
near(rigA._skinMatricesArray[13], 0, 'source skin translation y');
// Inverse bind pose must be the one captured at bind time.
near(rigA._invBindPoseMatricesArray[12], -1, 'hip invBind x');
near(rigA._invBindPoseMatricesArray[13], -2, 'hip invBind y');
near(rigA._invBindPoseMatricesArray[14], -3, 'hip invBind z');
near(rigA._invBindPoseMatricesArray[16 + 13], -3, 'limb invBind y');
// Source bounding box rebuilt from the posed skeleton: [-1,1] shifted by 3.
near(rigA.boundingBox.min.array[0], 2, 'source bbox min x');
near(rigA.boundingBox.max.array[0], 4, 'source bbox max x');

// ---------------------------------------------------------------------------
// Clone the same subtree twice
// ---------------------------------------------------------------------------

var scene = new Scene();
var clone1 = scene.cloneNode(actor);
var clone2 = scene.cloneNode(actor);

var c1Hip = findByName(clone1, 'hip');
var c1Limb = findByName(clone1, 'limb');
var c1Body = findByName(clone1, 'body');
var c1Coat = findByName(clone1, 'coat');
var c1Extra = findByName(clone1, 'extra');
var c1Extra2 = findByName(clone1, 'extra2');
var c1NullRootMesh = findByName(clone1, 'nullRootMesh');
var c1Statue = findByName(clone1, 'statue');
var c1Prop = findByName(clone1, 'prop');

var c2Body = findByName(clone2, 'body');
var c2Coat = findByName(clone2, 'coat');
var c2Extra = findByName(clone2, 'extra');

var c1RigA = c1Body.skeleton;
var c1RigB = c1Extra.skeleton;
var c1RigC = c1NullRootMesh.skeleton;
var c2RigA = c2Body.skeleton;

// --- Sharing is preserved by skeleton identity, per cloneNode call ---------

assert.equal(c1Coat.skeleton, c1RigA, 'meshes sharing a skeleton keep sharing the clone');
assert.equal(c1Extra2.skeleton, c1RigB, 'second mesh on rigB shares the same clone');
assert.notEqual(c1RigA, rigA, 'cloned skeleton is a new object');
assert.notEqual(c1RigB, rigB, 'cloned rigB is a new object');
assert.notEqual(c1RigA, c1RigB, 'same-named skeletons are not merged');
assert.equal(c1RigA.name, 'rig');
assert.equal(c1RigB.name, 'rig');
assert.notEqual(c2RigA, c1RigA, 'second cloneNode call produces its own skeleton');
assert.notEqual(c2RigA, rigA);
assert.equal(c2Coat.skeleton, c2RigA, 'sharing also holds in the second clone');
assert.notEqual(c2Extra.skeleton, c2RigA, 'same-named skeletons stay separate in clone2');

// --- Node references ---------------------------------------------------------

assert.equal(c1RigA.joints[0].node, c1Hip, 'in-tree joint node is remapped');
assert.equal(c1RigA.joints[1].node, c1Limb, 'in-tree joint node is remapped');
assert.notEqual(c1Hip, hip);
assert.equal(c1RigC.joints[0].node, c1Hip, 'two skeletons referencing one node stay consistent');
assert.equal(c1RigA.relativeRootNode, clone1, 'in-tree relativeRootNode is remapped');

assert.equal(c1RigB.joints[0].node, outsideBone, 'out-of-tree joint node keeps the original reference');
assert.equal(c1RigB.relativeRootNode, externalRoot, 'out-of-tree relativeRootNode keeps the original reference');
assert.equal(c1RigC.relativeRootNode, null, 'null relativeRootNode stays null');

// --- Per-mesh joints palettes ------------------------------------------------

assert.deepEqual(c1Body.joints, [0, 1]);
assert.deepEqual(c1Coat.joints, [1, 0]);
assert.notEqual(c1Body.joints, body.joints, 'each mesh keeps its own palette copy');
assert.notEqual(c1Coat.joints, coat.joints);

// --- Independent buffers, preserved bind pose --------------------------------

assert.notEqual(c1RigA._skinMatricesArray, rigA._skinMatricesArray);
assert.notEqual(c1RigA._skinMatricesArray, c2RigA._skinMatricesArray);
assert.notEqual(c1RigA._invBindPoseMatricesArray, rigA._invBindPoseMatricesArray);
assert.notEqual(
    c1RigA._skinMatricesSubArrays[0].buffer,
    rigA._skinMatricesSubArrays[0].buffer,
    'matrix sub views must not alias the source buffer'
);
near(c1RigA._invBindPoseMatricesArray[12], -1, 'clone preserves hip invBind x');
near(c1RigA._invBindPoseMatricesArray[16 + 13], -3, 'clone preserves limb invBind y');
near(c2RigA._invBindPoseMatricesArray[12], -1, 'clone2 preserves hip invBind x');

// --- Joint bounding boxes ----------------------------------------------------

assert.equal(c1RigA._jointsBoundingBoxes.length, 2);
assert.equal(c1RigA._jointsBoundingBoxes[0].__updated, true, 'valid joint bbox keeps its flag');
assert.equal(c1RigA._jointsBoundingBoxes[1].__updated, false, 'invalid joint bbox keeps its flag');
assert.notEqual(c1RigA._jointsBoundingBoxes[0], rigA._jointsBoundingBoxes[0]);
near(c1RigA._jointsBoundingBoxes[0].min.array[0], -1, 'joint bbox range preserved');
near(c1RigA._jointsBoundingBoxes[0].max.array[0], 1, 'joint bbox range preserved');
assert.ok(c1RigA.boundingBox, 'cloned skeleton has a bounding box');
assert.notEqual(c1RigA.boundingBox, rigA.boundingBox, 'bounding box is not shared with the source');

// ---------------------------------------------------------------------------
// Move only the first clone and verify skinning output
// ---------------------------------------------------------------------------

// Bring the second clone to its resting pose (world transforms of freshly
// cloned nodes are computed on the first update, like for any new node).
clone2.update();
c2RigA.update();
near(c2RigA._skinMatricesArray[12], 3, 'clone2 rests at the source pose');

clone1.position.x = 5;
c1Limb.position.y = 3; // was 1, makes joint 1 distinguishable from joint 0
clone1.update();
c1RigA.update();
c1RigB.update();
c1RigC.update();

// hip world x is now 4 + 5 = 9, bind x = 1 -> skin translation x = 8.
near(c1RigA._skinMatricesArray[12], 8, 'clone1 hip skin x');
near(c1RigA._skinMatricesArray[13], 0, 'clone1 hip skin y');
// limb moved an extra 2 on y (local y 1 -> 3 under hip at y = 2).
near(c1RigA._skinMatricesArray[16 + 12], 8, 'clone1 limb skin x');
near(c1RigA._skinMatricesArray[16 + 13], 2, 'clone1 limb skin y');

// Each palette sees the joints in its own order.
var bodySub = c1RigA.getSubSkinMatrices('body', c1Body.joints);
near(bodySub[12], 8, 'body palette joint0 x');
near(bodySub[13], 0, 'body palette joint0 y');
near(bodySub[16 + 12], 8, 'body palette joint1 x');
near(bodySub[16 + 13], 2, 'body palette joint1 y');
var coatSub = c1RigA.getSubSkinMatrices('coat', c1Coat.joints);
near(coatSub[12], 8, 'coat palette starts with joint1 x');
near(coatSub[13], 2, 'coat palette starts with joint1 y');
near(coatSub[16 + 12], 8, 'coat palette joint0 x');
near(coatSub[16 + 13], 0, 'coat palette joint0 y');

// Sub skin matrices must not borrow the source skeleton's writable buffers.
var srcSub = rigA.getSubSkinMatrices('body', body.joints);
assert.notEqual(bodySub, srcSub);
near(srcSub[12], 3, 'source palette buffer is not touched by the clone');

// The other copy and the source did not move.
near(rigA._skinMatricesArray[12], 3, 'source skeleton stays put');
near(rigA._skinMatricesArray[16 + 13], 0, 'source limb stays put');
near(c2RigA._skinMatricesArray[12], 3, 'clone2 stays put');
near(c2RigA._skinMatricesArray[16 + 13], 0, 'clone2 limb stays put');
near(hip.position.x, 4, 'source bone transform untouched');
near(hip.position.y, 2, 'source bone transform untouched');

// Bounding boxes follow the bones of each copy independently.
near(c1RigA.boundingBox.min.array[0], 7, 'clone1 bbox min x follows the moved bones');
near(c1RigA.boundingBox.max.array[0], 9, 'clone1 bbox max x follows the moved bones');
near(c1RigA.boundingBox.min.array[1], -1, 'clone1 bbox min y');
near(c1RigA.boundingBox.max.array[1], 1, 'clone1 bbox max y');
assert.ok(c1RigA.boundingBox.min.array[0] < c1RigA.boundingBox.max.array[0], 'clone1 bbox is not empty');
near(rigA.boundingBox.min.array[0], 2, 'source bbox is not rewritten by the clone');
near(rigA.boundingBox.max.array[0], 4, 'source bbox is not rewritten by the clone');
near(c2RigA.boundingBox.min.array[0], 2, 'clone2 bbox keeps its own pose');
near(c2RigA.boundingBox.max.array[0], 4, 'clone2 bbox keeps its own pose');

// ---------------------------------------------------------------------------
// Plain non-skinned nodes still clone as before
// ---------------------------------------------------------------------------

assert.notEqual(c1Prop, prop);
assert.equal(c1Prop.name, 'prop');
assert.equal(c1Prop._children.length, 1);
assert.equal(c1Prop._children[0].name, 'propChild');
assert.notEqual(c1Prop._children[0], propChild);
c1Prop._children[0].position.x = 7;
near(propChild.position.x, 0, 'moving a cloned plain node does not touch the source');

// geometry stays shared, material keeps the existing clone-per-mesh behavior.
assert.equal(c1Statue.geometry, statue.geometry, 'geometry is still shared');
assert.notEqual(c1Statue.material, statue.material, 'material is still cloned');

// ---------------------------------------------------------------------------
// The source tree must be left untouched
// ---------------------------------------------------------------------------

assert.equal(actor._children.length, 8, 'source children count unchanged');
assert.equal(actor._children[7], rigBranch, 'source children order unchanged');
assert.equal(rigBranch._children[0], hip, 'source parent/child relation unchanged');
assert.equal(hip._parent, rigBranch);
assert.equal(rigA.joints[0].node, hip, 'source skeleton still references source nodes');
assert.equal(rigA.relativeRootNode, actor);
assert.equal(rigB.relativeRootNode, externalRoot);

console.log('CLONE_SKELETON_OK');
