var System = require('../../entities/systems/System');

// has to stay here because it's used by traverseFunc below
// it's pretty crappy how it's sprinkled over the code
var numUpdates;

/**
 * Processes all entities with transform components, making sure they are up to date and valid according to the "scenegraph"
 * @example-link http://code.gooengine.com/latest/visual-test/goo/entities/components/TransformComponent/TransformComponent-vtest.html Working example
 * @extends System
 */
function TransformSystem() {
	System.call(this, 'TransformSystem', ['TransformComponent']);
	this.numUpdates = 0;
}

TransformSystem.prototype = Object.create(System.prototype);
TransformSystem.prototype.constructor = TransformSystem;

TransformSystem.prototype.process = function () {
	var entities = this._activeEntities;

	numUpdates = 0;
	var i, transformComponent;
	var l = entities.length;

	var residency = this.world && this.world._sceneResidency;
	if (residency && residency.isActive()) {
		this._processResidentMembers(entities, this.world.entityManager, l);
		this.numUpdates = numUpdates;
		return;
	}

	for (i = 0; i < l; i++) {
		transformComponent = entities[i].transformComponent;
		if (transformComponent._localTransformDirty) {
			transformComponent.updateTransform();
		}
	}

	// Traverse from root nodes and down, depth first
	for (i = 0; i < l; i++) {
		var entity = entities[i];
		transformComponent = entity.transformComponent;
		if (transformComponent.parent === null) {
			entity.traverse(traverseFunc);
		}
	}

	this.numUpdates = numUpdates;
};

/**
 * Update only entities that are currently members of the world. The residency
 * controller can keep ancestors of resident roots in the world while their
 * other branches are not resident; those branches must not be updated and must
 * not contribute to numUpdates.
 */
TransformSystem.prototype._processResidentMembers = function (entities, entityManager, length) {
	var i, transformComponent;

	for (i = 0; i < length; i++) {
		if (!entityManager.containsEntity(entities[i])) {
			continue;
		}
		transformComponent = entities[i].transformComponent;
		if (transformComponent._localTransformDirty) {
			transformComponent.updateTransform();
		}
	}

	for (i = 0; i < length; i++) {
		var entity = entities[i];
		if (!entityManager.containsEntity(entity)) {
			continue;
		}
		transformComponent = entity.transformComponent;
		var parent = transformComponent.parent;
		if (parent === null || !entityManager.containsEntity(parent.entity)) {
			traverseResident(entity, entityManager);
		}
	}
};

function traverseResident(entity, entityManager) {
	var transformComponent = entity.transformComponent;

	if (transformComponent._worldTransformDirty) {
		transformComponent.updateWorldTransform();
		numUpdates++;

		// Children that are not world members must not be marked dirty or
		// updated: they are outside the resident transform update.
		var children = transformComponent.children;
		for (var j = 0; j < children.length; j++) {
			if (entityManager.containsEntity(children[j].entity)) {
				children[j]._worldTransformDirty = true;
			}
		}
	}

	var childComponents = transformComponent.children;
	for (var k = 0; k < childComponents.length; k++) {
		var childEntity = childComponents[k].entity;
		if (entityManager.containsEntity(childEntity)) {
			traverseResident(childEntity, entityManager);
		}
	}
}

function traverseFunc(entity) {
	if (entity.transformComponent._worldTransformDirty) {
		entity.transformComponent.updateWorldTransform();
		numUpdates++;
		// Set children to dirty
		var children = entity.transformComponent.children;
		for (var j = 0; j < children.length; j++) {
			children[j]._worldTransformDirty = true;
		}
	}
}

module.exports = TransformSystem;
