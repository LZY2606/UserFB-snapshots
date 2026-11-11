var Component = require('../../entities/components/Component');
var Transform = require('../../math/Transform');

/**
 * Drives the transform of the entity it is attached to so that it follows a joint
 * of another entity's skeleton. The target entity keeps its place in the scene
 * graph; only its local transform is rewritten each frame.
 *
 * The desired world transform is computed as
 * <code>sourceWorld * jointGlobal * offset</code>, where jointGlobal is the joint's
 * model-space global transform from the current {@link SkeletonPose} (not the
 * matrix palette, which already contains the inverse bind pose). The result is
 * then converted into the target parent's local space.
 *
 * @extends Component
 * @param {Entity} sourceEntity Entity carrying the {@link AnimationComponent} whose pose provides the joint
 * @param {number} jointIndex Index of the joint in the {@link Skeleton}'s joint array
 * @param {Transform} [offset] Optional transform applied after the joint; stored as an independent copy, defaults to identity
 */
function JointAttachmentComponent(sourceEntity, jointIndex, offset) {
	Component.apply(this, arguments);

	this.type = 'JointAttachmentComponent';

	/**
	 * Entity whose AnimationComponent provides the current SkeletonPose.
	 * Replacing it takes effect on the next frame.
	 * @type {Entity}
	 */
	this.sourceEntity = sourceEntity;

	/**
	 * Index of the joint to attach to, as positioned in the Skeleton's joint array.
	 * @type {number}
	 */
	this.jointIndex = jointIndex;

	/**
	 * Independent copy of the offset transform. Defaults to the identity transform.
	 * @type {Transform}
	 */
	this.offset = new Transform();
	if (offset) {
		this.offset.copy(offset);
	}
}

JointAttachmentComponent.type = 'JointAttachmentComponent';

JointAttachmentComponent.prototype = Object.create(Component.prototype);
JointAttachmentComponent.prototype.constructor = JointAttachmentComponent;

/**
 * Replace the source entity. The new entity is read on the next frame.
 * @param {Entity} sourceEntity
 * @returns {JointAttachmentComponent} Self for chaining
 */
JointAttachmentComponent.prototype.setSourceEntity = function (sourceEntity) {
	this.sourceEntity = sourceEntity;
	return this;
};

/**
 * Replace the attachment offset. The supplied transform is copied.
 * @param {Transform} offset
 * @returns {JointAttachmentComponent} Self for chaining
 */
JointAttachmentComponent.prototype.setOffset = function (offset) {
	if (offset) {
		this.offset.copy(offset);
	} else {
		this.offset.setIdentity();
	}
	return this;
};

JointAttachmentComponent.prototype.clone = function () {
	return new JointAttachmentComponent(this.sourceEntity, this.jointIndex, this.offset);
};

module.exports = JointAttachmentComponent;
