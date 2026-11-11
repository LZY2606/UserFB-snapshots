var System = require('../../entities/systems/System');
var Matrix3 = require('../../math/Matrix3');
var Matrix4 = require('../../math/Matrix4');
var Vector3 = require('../../math/Vector3');

/**
 * Processes entities carrying a {@link JointAttachmentComponent} and drives their
 * local transform so that they follow a joint of another entity's skeleton.
 *
 * The system is meant to run after the {@link AnimationSystem} has applied the
 * current pose and before the {@link TransformSystem} and render collection run.
 * It does not listen to pose events: every frame re-evaluates the target world
 * matrix from the source entity's world matrix, the joint's model-space global
 * transform and the attachment offset, so moving the source root or the target's
 * parent while animation is paused still updates the attachment on the same frame.
 *
 * Attachments may be chained: an attachment target can itself be the source
 * entity, or an ancestor of the target parent, of another attachment. The update
 * order is resolved independently of the order in which entities were added to
 * the world. Cycles are rejected before any transform is written.
 *
 * @extends System
 */
function JointAttachmentSystem() {
	System.call(this, 'JointAttachmentSystem', ['JointAttachmentComponent']);

	/**
	 * Default priority. Register the systems with explicit priorities, e.g.
	 * AnimationSystem = -100, JointAttachmentSystem = -50, TransformSystem = 0.
	 * @type {number}
	 */
	this.priority = 0;
}

JointAttachmentSystem.prototype = Object.create(System.prototype);
JointAttachmentSystem.prototype.constructor = JointAttachmentSystem;

// Tolerances used when rejecting affine results that a Transform cannot represent.
var ORTHOGONALITY_EPSILON = 1e-7;
var DETERMINANT_EPSILON = 1e-10;
var RECONSTRUCTION_EPSILON = 1e-6;
var tmpWorldMatrix = new Matrix4();
var tmpJointMatrix = new Matrix4();
var tmpParentInverse = new Matrix4();
var tmpLocalMatrix = new Matrix4();
var tmpReconstructedMatrix = new Matrix4();
var tmpRotation = new Matrix3();
var tmpScale = new Vector3();

JointAttachmentSystem.prototype.process = function () {
	var entities = this._activeEntities;
	var count = entities.length;
	if (count === 0) {
		return;
	}

	var states = new Array(count); // 0 = unvisited, 1 = visiting, 2 = done
	for (var s = 0; s < count; s++) {
		states[s] = 0;
	}
	var order = [];

	// Phase 1: validate every attachment and resolve a dependency order without
	// writing any transforms. A cycle throws here, before phase 2 can mutate the scene.
	for (var i = 0; i < count; i++) {
		if (states[i] === 0) {
			this._visit(entities, states, order, i);
		}
	}

	// Phase 2: write the attachments in dependency order.
	for (var j = 0; j < order.length; j++) {
		this._applyAttachment(entities[order[j]]);
	}
};

/**
 * Depth-first visit used to topologically sort the attachments and detect cycles.
 * @private
 */
JointAttachmentSystem.prototype._visit = function (entities, states, order, index) {
	switch (states[index]) {
		case 1:
			throw new Error('JointAttachmentSystem: cyclic joint attachment dependency detected at entity "' +
				entities[index].name + '"');
		case 2:
			return;
	}
	states[index] = 1;

	var entity = entities[index];
	this._validate(entity);

	for (var i = 0; i < entities.length; i++) {
		if (i === index || states[i] === 2) {
			continue;
		}
		if (this._dependsOn(entity, entities[i])) {
			this._visit(entities, states, order, i);
		}
	}

	states[index] = 2;
	order.push(index);
};

/**
 * Returns true if the attachment on `entity` reads a transform written by the
 * attachment on `other`: the other target is the source itself or lies on the
 * parent chain of the source or of the entity's parent.
 * @private
 */
JointAttachmentSystem.prototype._dependsOn = function (entity, other) {
	var otherTransform = other.transformComponent;

	if (entity.jointAttachmentComponent.sourceEntity === other) {
		return true;
	}

	var source = entity.jointAttachmentComponent.sourceEntity;
	if (source && source.transformComponent && isAncestorOrSelf(otherTransform, source.transformComponent)) {
		return true;
	}

	var parent = entity.transformComponent.parent;
	if (parent && isAncestorOrSelf(otherTransform, parent)) {
		return true;
	}

	return false;
};

/**
 * Checks that everything needed to evaluate the attachment exists and is in range.
 * @private
 */
JointAttachmentSystem.prototype._validate = function (entity) {
	var attachment = entity.jointAttachmentComponent;
	var sourceEntity = attachment.sourceEntity;
	var jointIndex = attachment.jointIndex;

	if (!entity.transformComponent) {
		throw new Error('JointAttachmentSystem: target entity "' + entity.name +
			'" has no TransformComponent (joint index ' + jointIndex + ')');
	}

	if (!sourceEntity) {
		throw new Error('JointAttachmentSystem: missing source entity for attachment on entity "' + entity.name +
			'", joint index ' + jointIndex);
	}

	if (!sourceEntity.transformComponent) {
		throw new Error('JointAttachmentSystem: source entity "' + sourceEntity.name +
			'" has no TransformComponent (attachment on entity "' + entity.name + '", joint index ' + jointIndex + ')');
	}

	var animationComponent = sourceEntity.animationComponent;
	if (!animationComponent || !animationComponent._skeletonPose) {
		throw new Error('JointAttachmentSystem: source entity "' + sourceEntity.name +
			'" has no AnimationComponent with a SkeletonPose (attachment on entity "' + entity.name +
			'", joint index ' + jointIndex + ')');
	}

	var pose = animationComponent._skeletonPose;
	var jointCount = pose._globalTransforms.length;
	if (jointIndex < 0 || jointIndex >= jointCount) {
		throw new Error('JointAttachmentSystem: joint index ' + jointIndex + ' is out of range [0,' + jointCount +
			') on source entity "' + sourceEntity.name + '" (attachment on entity "' + entity.name + '")');
	}

	// A source inside the target's own subtree would depend on the transform this
	// attachment is about to write. Detect it even when no second attachment exists.
	if (isAncestorOrSelf(entity.transformComponent, sourceEntity.transformComponent)) {
		throw new Error('JointAttachmentSystem: cyclic joint attachment dependency: source entity "' +
			sourceEntity.name + '" is the target entity "' + entity.name + '" or one of its descendants (joint index ' +
			jointIndex + ')');
	}
};

/**
 * Computes and writes the local transform of a single attachment target.
 * @private
 */
JointAttachmentSystem.prototype._applyAttachment = function (entity) {
	var attachment = entity.jointAttachmentComponent;
	var sourceEntity = attachment.sourceEntity;
	var pose = sourceEntity.animationComponent._skeletonPose;
	var jointTransform = pose._globalTransforms[attachment.jointIndex];

	var sourceTransformComponent = sourceEntity.transformComponent;
	var targetTransformComponent = entity.transformComponent;

	// Make sure the source world matrix and the target parent world matrix are current.
	sourceTransformComponent.sync();
	if (targetTransformComponent.parent) {
		targetTransformComponent.parent.sync();
	}

	// desired world = source world * joint model-space global * offset
	tmpWorldMatrix.mul2(sourceTransformComponent.worldTransform.matrix, jointTransform.matrix);
	tmpJointMatrix.mul2(tmpWorldMatrix, attachment.offset.matrix);

	// convert into the actual parent's local space (identity parent when there is none)
	if (targetTransformComponent.parent) {
		var parentWorldMatrix = targetTransformComponent.parent.worldTransform.matrix;
		if (Math.abs(parentWorldMatrix.determinant()) < DETERMINANT_EPSILON) {
			throw new Error('JointAttachmentSystem: parent transform of target entity "' + entity.name +
				'" is singular and cannot be inverted');
		}
		tmpParentInverse.copy(parentWorldMatrix).invert();
		tmpLocalMatrix.mul2(tmpParentInverse, tmpJointMatrix);
	} else {
		tmpLocalMatrix.copy(tmpJointMatrix);
	}

	var transform = targetTransformComponent.transform;
	decomposeTRS(tmpLocalMatrix, transform.translation, tmpRotation, tmpScale);

	transform.matrix.copy(tmpLocalMatrix);
	transform.rotation.copy(tmpRotation);
	transform.scale.set(tmpScale);

	// push the result through the regular dirty/sync flow, including the whole subtree
	targetTransformComponent.setUpdated();
	syncSubtree(targetTransformComponent);
};

/**
 * Walks a transform component tree downwards, syncing world transforms and
 * refreshing world bounds on entities with both mesh data and mesh renderer components.
 * @private
 */
function syncSubtree(transformComponent) {
	transformComponent.sync();

	var entity = transformComponent.entity;
	if (entity && entity.meshDataComponent && entity.meshRendererComponent) {
		var meshDataComponent = entity.meshDataComponent;
		if (meshDataComponent.modelBoundDirty) {
			meshDataComponent.computeBoundFromPoints();
		}
		entity.meshRendererComponent.updateBounds(meshDataComponent.modelBound, transformComponent.worldTransform);
	}

	var children = transformComponent.children;
	for (var i = 0; i < children.length; i++) {
		syncSubtree(children[i]);
	}
}

/**
 * Returns true if `ancestor` is the same transform component as `component` or
 * sits somewhere on its parent chain.
 * @private
 */
function isAncestorOrSelf(ancestor, component) {
	var current = component;
	while (current) {
		if (current === ancestor) {
			return true;
		}
		current = current.parent;
	}
	return false;
}

/**
 * Decomposes an affine 4x4 matrix into translation, rotation and scale while
 * rejecting singular matrices, reflections and shear: only positive-determinant,
 * shear-free TRS transforms (non-uniform scale included) can be represented by
 * the engine's Transform class.
 * @private
 */
function decomposeTRS(matrix, translationStore, rotationStore, scaleStore) {
	var d = matrix.data;

	if (Math.abs(d[3]) > ORTHOGONALITY_EPSILON ||
		Math.abs(d[7]) > ORTHOGONALITY_EPSILON ||
		Math.abs(d[11]) > ORTHOGONALITY_EPSILON ||
		Math.abs(d[15] - 1) > ORTHOGONALITY_EPSILON) {
		throw new Error('JointAttachmentSystem: target transform is not an affine TRS matrix and cannot be represented by a Transform');
	}

	var c0x = d[0], c0y = d[1], c0z = d[2];
	var c1x = d[4], c1y = d[5], c1z = d[6];
	var c2x = d[8], c2y = d[9], c2z = d[10];

	var s0 = Math.sqrt(c0x * c0x + c0y * c0y + c0z * c0z);
	var s1 = Math.sqrt(c1x * c1x + c1y * c1y + c1z * c1z);
	var s2 = Math.sqrt(c2x * c2x + c2y * c2y + c2z * c2z);

	if (s0 < DETERMINANT_EPSILON || s1 < DETERMINANT_EPSILON || s2 < DETERMINANT_EPSILON) {
		throw new Error('JointAttachmentSystem: singular target transform cannot be represented by a Transform');
	}

	// positive determinant only: reflections (negative determinant) are rejected
	var det = (c0y * c1z - c0z * c1y) * c2x +
		(c0z * c1x - c0x * c1z) * c2y +
		(c0x * c1y - c0y * c1x) * c2z;
	if (det <= DETERMINANT_EPSILON) {
		throw new Error('JointAttachmentSystem: target transform has a non-positive determinant (reflection or singular matrix) and cannot be represented by a Transform');
	}

	var r0x = c0x / s0, r0y = c0y / s0, r0z = c0z / s0;
	var r1x = c1x / s1, r1y = c1y / s1, r1z = c1z / s1;
	var r2x = c2x / s2, r2y = c2y / s2, r2z = c2z / s2;

	// normalized columns must be pairwise orthogonal, otherwise the matrix is sheared
	if (Math.abs(r0x * r1x + r0y * r1y + r0z * r1z) > ORTHOGONALITY_EPSILON ||
		Math.abs(r0x * r2x + r0y * r2y + r0z * r2z) > ORTHOGONALITY_EPSILON ||
		Math.abs(r1x * r2x + r1y * r2y + r1z * r2z) > ORTHOGONALITY_EPSILON) {
		throw new Error('JointAttachmentSystem: sheared target transform cannot be represented by a Transform without changing its pose');
	}

	// guard against silently accepting a decomposition that reconstructs a different pose
	var reconstructed = tmpReconstructedMatrix;
	reconstructed.data[0] = s0 * r0x;
	reconstructed.data[1] = s0 * r0y;
	reconstructed.data[2] = s0 * r0z;
	reconstructed.data[3] = 0;
	reconstructed.data[4] = s1 * r1x;
	reconstructed.data[5] = s1 * r1y;
	reconstructed.data[6] = s1 * r1z;
	reconstructed.data[7] = 0;
	reconstructed.data[8] = s2 * r2x;
	reconstructed.data[9] = s2 * r2y;
	reconstructed.data[10] = s2 * r2z;
	reconstructed.data[11] = 0;
	reconstructed.data[12] = d[12];
	reconstructed.data[13] = d[13];
	reconstructed.data[14] = d[14];
	reconstructed.data[15] = 1;

	for (var i = 0; i < 16; i++) {
		// scale the tolerance for large translations, which legitimately carry more absolute error
		var tolerance = RECONSTRUCTION_EPSILON * (1 + Math.abs(d[i]));
		if (Math.abs(reconstructed.data[i] - d[i]) > tolerance) {
			throw new Error('JointAttachmentSystem: target transform cannot be reconstructed as a shear-free TRS Transform');
		}
	}

	// only write the stores once the decomposition is fully accepted
	var rd = rotationStore.data;
	rd[0] = r0x; rd[1] = r0y; rd[2] = r0z;
	rd[3] = r1x; rd[4] = r1y; rd[5] = r1z;
	rd[6] = r2x; rd[7] = r2y; rd[8] = r2z;

	scaleStore.setDirect(s0, s1, s2);
	translationStore.setDirect(d[12], d[13], d[14]);
}

module.exports = JointAttachmentSystem;
