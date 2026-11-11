'use strict';

// Node-only integration test for JointAttachmentComponent/JointAttachmentSystem.
// Runs a real World with AnimationSystem, JointAttachmentSystem, TransformSystem
// and BoundingUpdateSystem, driven by programmatically constructed SkeletonPoses.
// No WebGL required.

var assert = require('assert');

var Entity = require('../../../src/goo/entities/Entity');
var World = require('../../../src/goo/entities/World');
var TransformComponent = require('../../../src/goo/entities/components/TransformComponent');
var MeshDataComponent = require('../../../src/goo/entities/components/MeshDataComponent');
var MeshRendererComponent = require('../../../src/goo/entities/components/MeshRendererComponent');

var Transform = require('../../../src/goo/math/Transform');
var Matrix4 = require('../../../src/goo/math/Matrix4');
var Vector3 = require('../../../src/goo/math/Vector3');
var BoundingBox = require('../../../src/goo/renderer/bounds/BoundingBox');

var Joint = require('../../../src/goo/animationpack/Joint');
var Skeleton = require('../../../src/goo/animationpack/Skeleton');
var SkeletonPose = require('../../../src/goo/animationpack/SkeletonPose');
var AnimationComponent = require('../../../src/goo/animationpack/components/AnimationComponent');
var AnimationSystem = require('../../../src/goo/animationpack/systems/AnimationSystem');
var JointAttachmentComponent = require('../../../src/goo/animationpack/components/JointAttachmentComponent');
var JointAttachmentSystem = require('../../../src/goo/animationpack/systems/JointAttachmentSystem');
var TransformSystem = require('../../../src/goo/entities/systems/TransformSystem');
var BoundingUpdateSystem = require('../../../src/goo/entities/systems/BoundingUpdateSystem');

var EPS = 1e-6;

function makeEntity(world, name) {
	var entity = new Entity(world, name);
	entity.setComponent(new TransformComponent());
	return entity;
}

function translationMatrix(x, y, z) {
	var transform = new Transform();
	transform.translation.setDirect(x, y, z);
	transform.update();
	return transform.matrix.clone();
}

function matrixOf(transform) {
	transform.update();
	return transform.matrix.clone();
}

function assertMatrixClose(actual, expected, message) {
	assert.equal(actual.data.length, expected.data.length);
	for (var i = 0; i < expected.data.length; i++) {
		assert.ok(Math.abs(actual.data[i] - expected.data[i]) <= EPS,
			(message || 'matrix mismatch') + ' at element ' + i + ': ' + actual.data[i] + ' !== ' + expected.data[i]);
	}
}

function assertVecClose(actual, expected, message) {
	assert.ok(Math.abs(actual.x - expected.x) <= EPS &&
		Math.abs(actual.y - expected.y) <= EPS &&
		Math.abs(actual.z - expected.z) <= EPS,
		(message || 'vector mismatch') + ': (' + actual.x + ',' + actual.y + ',' + actual.z +
		') !== (' + expected.x + ',' + expected.y + ',' + expected.z + ')');
}

// Two joints; the hand joint has a non-identity inverse bind pose (inverse of T(0,2,0)).
function buildSkeleton() {
	var root = new Joint('root');
	root._index = 0;

	var hand = new Joint('hand');
	hand._index = 1;
	hand._parentIndex = 0;
	hand._inverseBindPose.translation.setDirect(0, -2, 0);
	hand._inverseBindPose.update();

	return new Skeleton('rig', [root, hand]);
}

function setHandLocal(pose, x, y, z) {
	var localTransform = pose._localTransforms[1];
	localTransform.translation.setDirect(x, y, z);
	localTransform.update();
	pose.updateTransforms();
}

function makeWorld() {
	var world = new World();
	world.registerComponent(AnimationComponent);
	world.registerComponent(JointAttachmentComponent);
	world.registerComponent(MeshDataComponent);
	world.registerComponent(MeshRendererComponent);

	var animationSystem = new AnimationSystem();
	animationSystem.priority = -100;

	var jointAttachmentSystem = new JointAttachmentSystem();
	jointAttachmentSystem.priority = -50;

	var transformSystem = new TransformSystem();
	transformSystem.priority = 0;

	var boundingUpdateSystem = new BoundingUpdateSystem();
	boundingUpdateSystem.priority = 50;

	world.setSystem(animationSystem);
	world.setSystem(jointAttachmentSystem);
	world.setSystem(transformSystem);
	world.setSystem(boundingUpdateSystem);

	return world;
}

function makeActor(world, name, pose, x, y, z) {
	var actor = makeEntity(world, name);
	actor.setComponent(new AnimationComponent(pose));
	actor.setTranslation(x, y, z);
	return actor;
}

// Only run when executed directly with node; other test runners that match
// *-test.js should not execute this node-only integration script.
if (typeof require !== 'undefined' && require.main === module) {
	runTests();
	console.log('JOINT_ATTACHMENT_OK');
}

function runTests() {
// ---------------------------------------------------------------------------
// Construction contract
// ---------------------------------------------------------------------------

(function testConstruction() {
	var world = new World();
	var pose = new SkeletonPose(buildSkeleton());
	var actor = makeActor(world, 'constructor-actor', pose, 0, 0, 0);

	var defaultAttachment = new JointAttachmentComponent(actor, 1);
	assert.equal(defaultAttachment.sourceEntity, actor);
	assert.equal(defaultAttachment.jointIndex, 1);
	assertMatrixClose(defaultAttachment.offset.matrix, new Matrix4(), 'default offset must be identity');

	var offset = new Transform();
	offset.rotation.fromAngles(0, 0, Math.PI / 5);
	offset.scale.setDirect(2, 3, 4);
	offset.update();

	var attachment = new JointAttachmentComponent(actor, 1, offset);
	// the component must keep an independent copy
	offset.rotation.setIdentity();
	offset.scale.setDirect(9, 9, 9);
	offset.update();

	assert.ok(Math.abs(attachment.offset.scale.x - 2) <= EPS);
	assert.ok(Math.abs(attachment.offset.scale.y - 3) <= EPS);
	assert.ok(Math.abs(attachment.offset.scale.z - 4) <= EPS);
	assert.ok(Math.abs(attachment.offset.rotation.data[0] - Math.cos(Math.PI / 5)) <= EPS);
}());

// ---------------------------------------------------------------------------
// Separate parent hierarchy, offset rotation, paused root movement, removal,
// source replacement, world matrix / child / bounds assertions
// ---------------------------------------------------------------------------

(function testMainLifecycle() {
	var world = makeWorld();
	var pose = new SkeletonPose(buildSkeleton());
	setHandLocal(pose, 0, 3, 0); // hand model-space global at (0,3,0)

	var actor = makeActor(world, 'actor', pose, 10, 0, 0);
	actor.addToWorld();

	// independent prop hierarchy: translated, rotated and scaled
	var propRoot = makeEntity(world, 'propRoot');
	propRoot.setTranslation(5, 0, 0);
	propRoot.setRotation(0, 0, Math.PI / 2);
	propRoot.setScale(2, 2, 2);
	propRoot.addToWorld();

	var attachment = makeEntity(world, 'attachment');
	propRoot.attachChild(attachment);

	var child = makeEntity(world, 'attachmentChild');
	child.setTranslation(1, 0, 0);
	attachment.attachChild(child);
	child.addToWorld();

	// local unit box bound, no mesh data needed
	var meshDataComponent = new MeshDataComponent(null);
	meshDataComponent.setModelBound(new BoundingBox(new Vector3(0, 0, 0), 1, 1, 1), false);
	var meshRendererComponent = new MeshRendererComponent();
	attachment.setComponent(meshDataComponent);
	attachment.setComponent(meshRendererComponent);

	attachment.setComponent(new JointAttachmentComponent(actor, 1));
	attachment.addToWorld();

	// independently built expected matrices
	var propRootWorld = new Matrix4()
		.mul2(translationMatrix(5, 0, 0), matrixOf((function () {
			var t = new Transform();
			t.rotation.fromAngles(0, 0, Math.PI / 2);
			return t;
		}())))
		.mul(matrixOf((function () {
			var t = new Transform();
			t.scale.setDirect(2, 2, 2);
			return t;
		}())));

	var expectedWorld = new Matrix4()
		.mul2(translationMatrix(10, 0, 0), pose._globalTransforms[1].matrix);

	world.process();

	// target world matrix follows source world * joint global * offset
	assertMatrixClose(attachment.transformComponent.sync().worldTransform.matrix, expectedWorld,
		'target world matrix');

	// local transform must be expressed in the actual parent's space
	var expectedLocal = new Matrix4().mul2(
		new Matrix4().copy(propRootWorld).invert(),
		expectedWorld
	);
	assertMatrixClose(attachment.transformComponent.transform.matrix, expectedLocal,
		'target local matrix in rotated/scaled parent space');

	// child subtree world transform is immediately readable
	var expectedChildWorld = new Matrix4().mul2(expectedWorld, translationMatrix(1, 0, 0));
	assertMatrixClose(child.transformComponent.sync().worldTransform.matrix, expectedChildWorld,
		'child world matrix');
	assertVecClose(child.transformComponent.worldTransform.translation, new Vector3(11, 3, 0),
		'child world translation');

	// world bounds of the MeshRendererComponent node are up to date
	assert.ok(meshRendererComponent.worldBound, 'world bound must exist');
	assertVecClose(meshRendererComponent.worldBound.center, new Vector3(10, 3, 0), 'world bound center');
	assert.ok(Math.abs(meshRendererComponent.worldBound.xExtent - 1) <= EPS);
	assert.ok(Math.abs(meshRendererComponent.worldBound.yExtent - 1) <= EPS);
	assert.ok(Math.abs(meshRendererComponent.worldBound.zExtent - 1) <= EPS);

	// --- offset rotation, still under the rotated/scaled prop hierarchy ---
	var offset = new Transform();
	offset.rotation.fromAngles(0, 0, Math.PI / 4);
	offset.update();
	attachment.jointAttachmentComponent.setOffset(offset);

	var expectedRotatedWorld = new Matrix4()
		.mul2(translationMatrix(10, 0, 0), pose._globalTransforms[1].matrix)
		.mul(matrixOf(offset));

	world.process();
	assertMatrixClose(attachment.transformComponent.sync().worldTransform.matrix, expectedRotatedWorld,
		'rotated offset world matrix');
	assertVecClose(attachment.transformComponent.worldTransform.translation, new Vector3(10, 3, 0),
		'rotated offset keeps joint translation');

	// --- animation paused: moving the root and/or parent must still be followed ---
	actor.animationComponent.pause();
	actor.setTranslation(20, 0, 0);
	propRoot.setTranslation(7, 0, 0);

	world.process();

	var expectedPausedWorld = new Matrix4()
		.mul2(translationMatrix(20, 0, 0), pose._globalTransforms[1].matrix)
		.mul(matrixOf(offset));
	assertMatrixClose(attachment.transformComponent.sync().worldTransform.matrix, expectedPausedWorld,
		'paused animation still follows moved root and parent');
	assertVecClose(attachment.transformComponent.worldTransform.translation, new Vector3(20, 3, 0),
		'paused follow translation');

	// --- removing the component stops driving but keeps the last pose ---
	var frozenMatrix = attachment.transformComponent.sync().worldTransform.matrix.clone();
	attachment.clearComponent('JointAttachmentComponent');
	world.process(); // process entity removal

	actor.setTranslation(40, 0, 0);
	world.process();
	assertMatrixClose(attachment.transformComponent.sync().worldTransform.matrix, frozenMatrix,
		'removed attachment keeps its last pose');

	// --- replacing sourceEntity: next frame reads the new character ---
	var pose2 = new SkeletonPose(buildSkeleton());
	setHandLocal(pose2, 0, 7, 0);
	var actor2 = makeActor(world, 'actor2', pose2, 0, 0, 0);
	actor2.addToWorld();

	attachment.setComponent(new JointAttachmentComponent(actor, 1, offset));
	world.process();
	assertVecClose(attachment.transformComponent.sync().worldTransform.translation, new Vector3(40, 3, 0),
		're-added attachment follows original source');

	attachment.jointAttachmentComponent.setSourceEntity(actor2);
	world.process();
	assertVecClose(attachment.transformComponent.sync().worldTransform.translation, new Vector3(0, 7, 0),
		'replaced source is read on the next frame');
}());

// ---------------------------------------------------------------------------
// Two-level chain, entities added to the world in reverse dependency order
// ---------------------------------------------------------------------------

(function testChainedReverseOrder() {
	var world = makeWorld();

	var pose1 = new SkeletonPose(buildSkeleton());
	setHandLocal(pose1, 0, 3, 0);
	var actor = makeActor(world, 'chain-actor', pose1, 10, 0, 0);

	// level 1: attachment target AND source character for level 2
	var level1 = makeEntity(world, 'level1');
	level1.setComponent(new JointAttachmentComponent(actor, 1));
	var pose2 = new SkeletonPose(buildSkeleton());
	setHandLocal(pose2, 0, 0, 5);
	level1.setComponent(new AnimationComponent(pose2));

	var pivot = makeEntity(world, 'pivot');
	pivot.setTranslation(1, 0, 0);
	level1.attachChild(pivot);

	var level2 = makeEntity(world, 'level2');
	pivot.attachChild(level2);
	level2.setComponent(new JointAttachmentComponent(level1, 1));

	// reverse order: deepest target first, source last, no recursive shortcuts
	world.addEntity(level2, false);
	world.addEntity(pivot, false);
	world.addEntity(level1, false);
	world.addEntity(actor, false);

	world.process();

	// level1 world = actor world (10,0,0) * hand1 global (0,3,0)
	assertVecClose(level1.transformComponent.sync().worldTransform.translation,
		new Vector3(10, 3, 0), 'level1 world translation');

	// level2 world = level1 world * hand2 global (0,0,5), pivot compensated in local space
	assertVecClose(level2.transformComponent.sync().worldTransform.translation,
		new Vector3(10, 3, 5), 'level2 world translation');

	var expectedLevel2 = new Matrix4()
		.mul2(translationMatrix(10, 0, 0), pose1._globalTransforms[1].matrix)
		.mul(pose2._globalTransforms[1].matrix);
	assertMatrixClose(level2.transformComponent.sync().worldTransform.matrix, expectedLevel2,
		'chained attachment world matrix');

	// second run must remain stable and order-independent
	world.process();
	assertMatrixClose(level2.transformComponent.sync().worldTransform.matrix, expectedLevel2,
		'stable chained result on second frame');
}());

// ---------------------------------------------------------------------------
// Dependency through the target parent chain (one target is an ancestor of
// another target's parent), entities added in reverse order
// ---------------------------------------------------------------------------

(function testTargetParentAncestorDependency() {
	var world = makeWorld();

	var pose1 = new SkeletonPose(buildSkeleton());
	setHandLocal(pose1, 0, 3, 0);
	var actor = makeActor(world, 'ancestor-actor', pose1, 10, 0, 0);

	var pose2 = new SkeletonPose(buildSkeleton());
	setHandLocal(pose2, 0, 0, 5);
	var actor2 = makeActor(world, 'ancestor-actor2', pose2, 0, 0, 20);

	// target1 drives itself into actor's hand and is an ancestor of target2's parent
	var target1 = makeEntity(world, 'ancestor-target1');
	target1.setComponent(new JointAttachmentComponent(actor, 1));

	var pivot = makeEntity(world, 'ancestor-pivot');
	pivot.setTranslation(1, 0, 0);
	target1.attachChild(pivot);

	var target2 = makeEntity(world, 'ancestor-target2');
	pivot.attachChild(target2);
	target2.setComponent(new JointAttachmentComponent(actor2, 1));

	// reverse order: double attachment first, then its ancestor, then the sources
	world.addEntity(target2, false);
	world.addEntity(pivot, false);
	world.addEntity(target1, false);
	world.addEntity(actor, false);
	world.addEntity(actor2, false);

	world.process();

	assertVecClose(target1.transformComponent.sync().worldTransform.translation,
		new Vector3(10, 3, 0), 'ancestor target world');
	assertVecClose(target2.transformComponent.sync().worldTransform.translation,
		new Vector3(0, 0, 25), 'descendant target world');
	assertMatrixClose(target2.transformComponent.worldTransform.matrix,
		translationMatrix(0, 0, 25), 'descendant target world matrix');

	// local matrix compensated for the already-updated ancestor target
	var expectedLocal = new Matrix4().mul2(
		new Matrix4().copy(translationMatrix(11, 3, 0)).invert(),
		translationMatrix(0, 0, 25)
	);
	assertMatrixClose(target2.transformComponent.transform.matrix, expectedLocal,
		'descendant target local matrix after ancestor update');
}());

// ---------------------------------------------------------------------------
// Target directly parented to the source is valid (it is a descendant, not a
// cycle: the source world does not depend on the target local transform)
// ---------------------------------------------------------------------------

(function testTargetChildOfSource() {
	var world = makeWorld();
	var pose = new SkeletonPose(buildSkeleton());
	setHandLocal(pose, 0, 3, 0);
	var actor = makeActor(world, 'parent-actor', pose, 10, 0, 0);
	actor.addToWorld();

	var target = makeEntity(world, 'parent-target');
	actor.attachChild(target);
	target.setComponent(new JointAttachmentComponent(actor, 1));
	target.addToWorld();

	world.process();

	assertVecClose(target.transformComponent.sync().worldTransform.translation,
		new Vector3(10, 3, 0), 'child-of-source world translation');
	// local space is relative to the source, so only the joint global remains
	assertMatrixClose(target.transformComponent.transform.matrix,
		pose._globalTransforms[1].matrix, 'child-of-source local matrix');
}());

// ---------------------------------------------------------------------------
// Representable non-uniform scale on a root-level target
// ---------------------------------------------------------------------------

(function testNonUniformScale() {
	var world = makeWorld();
	var pose = new SkeletonPose(buildSkeleton());
	setHandLocal(pose, 0, 3, 0);
	var actor = makeActor(world, 'scale-actor', pose, 10, 0, 0);
	actor.addToWorld();

	var offset = new Transform();
	offset.rotation.fromAngles(0, 0, Math.PI / 6);
	offset.scale.setDirect(2, 3, 4);
	offset.update();

	var target = makeEntity(world, 'scale-target'); // no parent
	target.setComponent(new JointAttachmentComponent(actor, 1, offset));
	target.addToWorld();

	world.process();

	var expected = new Matrix4()
		.mul2(translationMatrix(10, 0, 0), pose._globalTransforms[1].matrix)
		.mul(matrixOf(offset));

	assertMatrixClose(target.transformComponent.sync().worldTransform.matrix, expected,
		'non-uniform scale world matrix');
	assertVecClose(target.transformComponent.transform.scale, new Vector3(2, 3, 4),
		'non-uniform local scale');
}());

// ---------------------------------------------------------------------------
// Shear rejection: non-uniform parent scale combined with target rotation
// produces a local matrix that no Transform can represent
// ---------------------------------------------------------------------------

(function testShearRejection() {
	var world = makeWorld();
	var pose = new SkeletonPose(buildSkeleton());
	setHandLocal(pose, 0, 3, 0);
	var actor = makeActor(world, 'shear-actor', pose, 0, 0, 0);
	actor.addToWorld();

	var scaledParent = makeEntity(world, 'shear-parent');
	scaledParent.setScale(2, 1, 1);
	scaledParent.addToWorld();

	var target = makeEntity(world, 'shear-target');
	scaledParent.attachChild(target);

	var offset = new Transform();
	offset.rotation.fromAngles(0, 0, Math.PI / 4);
	offset.update();
	target.setComponent(new JointAttachmentComponent(actor, 1, offset));
	target.addToWorld();

	var before = target.transformComponent.transform.matrix.clone();
	assert.throws(function () {
		world.process();
	}, /shear|TRS/);

	// the rejected transform must not have been partially written
	assertMatrixClose(target.transformComponent.transform.matrix, before,
		'sheared result must not be written');
}());

// ---------------------------------------------------------------------------
// Source character under a rotated/scaled ordinary hierarchy
// ---------------------------------------------------------------------------

(function testSourceHierarchy() {
	var world = makeWorld();
	var pose = new SkeletonPose(buildSkeleton());
	setHandLocal(pose, 0, 3, 0);

	var sourceRoot = makeEntity(world, 'source-root');
	sourceRoot.setTranslation(3, 0, 0);
	sourceRoot.setRotation(0, 0, Math.PI / 2);
	sourceRoot.setScale(1, 2, 1);
	sourceRoot.addToWorld();

	var actor = makeActor(world, 'nested-actor', pose, 10, 0, 0);
	sourceRoot.attachChild(actor);
	actor.addToWorld();

	var target = makeEntity(world, 'nested-target'); // no parent
	target.setComponent(new JointAttachmentComponent(actor, 1));
	target.addToWorld();

	var sourceRootLocal = new Matrix4()
		.mul2(translationMatrix(3, 0, 0), matrixOf((function () {
			var t = new Transform();
			t.rotation.fromAngles(0, 0, Math.PI / 2);
			return t;
		}())))
		.mul(matrixOf((function () {
			var t = new Transform();
			t.scale.setDirect(1, 2, 1);
			return t;
		}())));

	var actorWorld = new Matrix4().mul2(sourceRootLocal, translationMatrix(10, 0, 0));
	var expected = new Matrix4().mul2(actorWorld, pose._globalTransforms[1].matrix);

	world.process();
	assertMatrixClose(target.transformComponent.sync().worldTransform.matrix, expected,
		'source under rotated/scaled hierarchy');
}());

// ---------------------------------------------------------------------------
// Reflection and singular rejection
// ---------------------------------------------------------------------------

(function testReflectionAndSingularRejection() {
	function runWithOffsetScale(scale, expectedMessage) {
		var world = makeWorld();
		var pose = new SkeletonPose(buildSkeleton());
		setHandLocal(pose, 0, 3, 0);
		var actor = makeActor(world, 'reject-actor', pose, 10, 0, 0);
		actor.addToWorld();

		var offset = new Transform();
		offset.scale.setDirect(scale[0], scale[1], scale[2]);
		offset.update();

		var target = makeEntity(world, 'reject-target');
		target.setComponent(new JointAttachmentComponent(actor, 1, offset));
		target.addToWorld();

		var before = target.transformComponent.transform.matrix.clone();
		assert.throws(function () {
			world.process();
		}, expectedMessage);
		assertMatrixClose(target.transformComponent.transform.matrix, before,
			'rejected transform must not be written');
	}

	// negative determinant: reflection
	runWithOffsetScale([-2, 3, 4], /determinant|reflection/i);
	// zero scale: singular
	runWithOffsetScale([0, 3, 4], /singular|determinant/i);
}());

// ---------------------------------------------------------------------------
// Cycle rejection: source inside target subtree, and mutually dependent targets
// ---------------------------------------------------------------------------

(function testCycleRejection() {
	function runStructuralCycle() {
		var world = makeWorld();
		var pose = new SkeletonPose(buildSkeleton());
		setHandLocal(pose, 0, 3, 0);
		var actor = makeActor(world, 'structural-actor', pose, 0, 0, 0);

		// target owns the only attachment; its source lives inside its own subtree
		var target = makeEntity(world, 'structural-target');
		target.addToWorld();
		var descendant = makeEntity(world, 'structural-descendant');
		descendant.setComponent(new AnimationComponent(new SkeletonPose(buildSkeleton())));
		target.attachChild(descendant);
		descendant.addToWorld();

		target.setComponent(new JointAttachmentComponent(descendant, 1));
		// keep actor referenced so the scenario is otherwise fully valid
		actor.addToWorld();

		assert.throws(function () {
			world.process();
		}, /cyclic/i);
	}

	// case 1: source character is a descendant of the target
	(function sourceDescendant() {
		var world = makeWorld();
		var pose = new SkeletonPose(buildSkeleton());
		setHandLocal(pose, 0, 3, 0);
		var actor = makeActor(world, 'cycle-actor', pose, 0, 0, 0);
		actor.addToWorld();

		var target = makeEntity(world, 'cycle-target');
		target.addToWorld();
		var descendant = makeEntity(world, 'cycle-descendant');
		target.attachChild(descendant);
		descendant.setComponent(new AnimationComponent(new SkeletonPose(buildSkeleton())));
		descendant.addToWorld();

		target.setComponent(new JointAttachmentComponent(descendant, 1));
		descendant.setComponent(new JointAttachmentComponent(actor, 1));

		assert.throws(function () {
			world.process();
		}, /cyclic/i);
	}());

	runStructuralCycle();

	// case 2: two attachments depend on each other
	(function mutualDependency() {
		var world = makeWorld();
		var poseA = new SkeletonPose(buildSkeleton());
		var poseB = new SkeletonPose(buildSkeleton());
		setHandLocal(poseA, 0, 3, 0);
		setHandLocal(poseB, 0, 0, 5);

		var entityA = makeEntity(world, 'mutual-A');
		var entityB = makeEntity(world, 'mutual-B');
		entityA.setComponent(new AnimationComponent(poseA));
		entityB.setComponent(new AnimationComponent(poseB));
		entityA.setComponent(new JointAttachmentComponent(entityB, 1));
		entityB.setComponent(new JointAttachmentComponent(entityA, 1));
		entityA.addToWorld();
		entityB.addToWorld();

		assert.throws(function () {
			world.process();
		}, /cyclic/i);
	}());
}());

// ---------------------------------------------------------------------------
// Error reporting: joint out of range, missing pose, missing transform
// ---------------------------------------------------------------------------

(function testErrorReporting() {
	var world = makeWorld();
	var pose = new SkeletonPose(buildSkeleton());
	var actor = makeActor(world, 'error-actor', pose, 0, 0, 0);
	actor.addToWorld();

	var outOfRange = makeEntity(world, 'out-of-range');
	outOfRange.setComponent(new JointAttachmentComponent(actor, 99));
	outOfRange.addToWorld();

	assert.throws(function () {
		world.process();
	}, function (error) {
		return /99/.test(error.message) && /error-actor/.test(error.message) &&
			/out-of-range/.test(error.message);
	});

	// source without AnimationComponent / pose
	var noPoseWorld = makeWorld();
	var bareSource = makeEntity(noPoseWorld, 'bare-source');
	bareSource.addToWorld();
	var noPoseTarget = makeEntity(noPoseWorld, 'no-pose-target');
	noPoseTarget.setComponent(new JointAttachmentComponent(bareSource, 0));
	noPoseTarget.addToWorld();

	assert.throws(function () {
		noPoseWorld.process();
	}, function (error) {
		return /SkeletonPose/.test(error.message) && /bare-source/.test(error.message) &&
			/no-pose-target/.test(error.message) && /0/.test(error.message);
	});

	// target without a TransformComponent
	var noTargetTransformWorld = makeWorld();
	var sourceWithPose = makeActor(noTargetTransformWorld, 'pose-source', new SkeletonPose(buildSkeleton()), 0, 0, 0);
	sourceWithPose.addToWorld();
	var bareTarget = new Entity(noTargetTransformWorld, 'bare-target');
	bareTarget.setComponent(new JointAttachmentComponent(sourceWithPose, 1));
	bareTarget.addToWorld();

	assert.throws(function () {
		noTargetTransformWorld.process();
	}, function (error) {
		return /TransformComponent/.test(error.message) && /bare-target/.test(error.message) &&
			/1/.test(error.message);
	});

	// source without TransformComponent
	var noTransformWorld = makeWorld();
	var ghost = new Entity(noTransformWorld, 'ghost-source');
	ghost.setComponent(new AnimationComponent(new SkeletonPose(buildSkeleton())));
	ghost.addToWorld();
	var ghostTarget = makeEntity(noTransformWorld, 'ghost-target');
	ghostTarget.setComponent(new JointAttachmentComponent(ghost, 1));
	ghostTarget.addToWorld();

	assert.throws(function () {
		noTransformWorld.process();
	}, function (error) {
		return /TransformComponent/.test(error.message) && /ghost-source/.test(error.message) &&
			/1/.test(error.message);
	});
}());
}
