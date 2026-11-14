'use strict';

/**
 * Keeps a spatial set of regions resident in a {@link World}.
 *
 * A region is selected by the distance from the observation points to its
 * world-space bounds. Selection uses hysteresis, and the selected regions are
 * expanded through their dependency graph. All entities belonging to the
 * selected regions' transform subtrees (plus the ancestors needed to preserve
 * their world transforms) are committed to the world as one synchronous
 * membership update.
 *
 * The residency controller never detaches or reparents entities: leaving the
 * world only removes the entity from the world's managers and systems.
 *
 * @param {World} world
 * @param {Array<{id: (string|number), roots: Array<Entity>, bounds: {min: Array<number>, max: Array<number>}, enter: number, leave: number, depends: Array<(string|number)>}>} regions
 */
function SceneResidency(world, regions) {
	if (!world || !world.entityManager ||
		typeof world.entityManager.getEntities !== 'function' ||
		typeof world.entityManager.containsEntity !== 'function') {
		throw new Error('SceneResidency: a World with an EntityManager is required');
	}

	var normalizedRegions = validateRegions(regions);

	if (world._sceneResidency) {
		throw new Error('SceneResidency: this World already has a SceneResidency controller');
	}

	this._world = world;

	this._regionById = new Map();
	this._regions = normalizedRegions.map(function (region) {
		var copy = {
			id: region.id,
			roots: region.roots.slice(),
			bounds: region.bounds,
			enter: region.enter,
			leave: region.leave,
			depends: region.depends.slice(),
			direct: false
		};
		return copy;
	}, this);

	this._regions.forEach(function (region) {
		this._regionById.set(region.id, region);
	}, this);

	// Entities that were already in the world when the controller was created.
	// They are fixed external members and are never removed by this controller.
	this._external = new Set(world.entityManager.getEntities());

	// Entities this controller added and that are currently resident.
	this._owned = new Set();

	this._disposed = false;

	world._sceneResidency = this;
}

function sceneResidencyError(message) {
	throw new Error('SceneResidency: ' + message);
}

function isFiniteNumber(value) {
	return typeof value === 'number' && isFinite(value);
}

function validateTriple(value, regionId, name) {
	if (!Array.isArray(value) || value.length !== 3) {
		sceneResidencyError('region "' + regionId + '" bounds.' + name + ' must be an array of three numbers');
	}
	for (var i = 0; i < 3; i++) {
		if (!isFiniteNumber(value[i])) {
			sceneResidencyError('region "' + regionId + '" bounds.' + name + '[' + i + '] is not finite');
		}
	}
}

function validateRegions(regions) {
	if (!Array.isArray(regions)) {
		sceneResidencyError('regions must be an array');
	}

	var byId = new Map();
	var i, j;

	for (i = 0; i < regions.length; i++) {
		var region = regions[i];
		if (!region || typeof region !== 'object') {
			sceneResidencyError('region at index ' + i + ' is not an object');
		}

		var id = region.id;
		if (typeof id !== 'string' && typeof id !== 'number') {
			sceneResidencyError('region at index ' + i + ' has an invalid id');
		}
		if (byId.has(id)) {
			sceneResidencyError('duplicate region id "' + id + '"');
		}

		if (!Array.isArray(region.roots)) {
			sceneResidencyError('region "' + id + '" roots must be an array');
		}
		for (j = 0; j < region.roots.length; j++) {
			var root = region.roots[j];
			if (!root || typeof root !== 'object' || !root.transformComponent ||
				typeof root.transformComponent !== 'object') {
				sceneResidencyError('region "' + id + '" has a missing or invalid root at index ' + j);
			}
		}

		var bounds = region.bounds;
		if (!bounds || typeof bounds !== 'object') {
			sceneResidencyError('region "' + id + '" is missing bounds');
		}
		validateTriple(bounds.min, id, 'min');
		validateTriple(bounds.max, id, 'max');
		for (j = 0; j < 3; j++) {
			if (bounds.min[j] > bounds.max[j]) {
				sceneResidencyError('region "' + id + '" has an inverted bounds box on axis ' + j);
			}
		}

		if (!isFiniteNumber(region.enter) || !isFiniteNumber(region.leave)) {
			sceneResidencyError('region "' + id + '" enter and leave must be finite numbers');
		}
		if (region.enter < 0) {
			sceneResidencyError('region "' + id + '" enter must not be negative');
		}
		if (region.enter > region.leave) {
			sceneResidencyError('region "' + id + '" enter must not be greater than leave');
		}

		var depends = region.depends;
		if (depends === undefined) {
			depends = [];
		}
		if (!Array.isArray(depends)) {
			sceneResidencyError('region "' + id + '" depends must be an array');
		}
		for (j = 0; j < depends.length; j++) {
			if (typeof depends[j] !== 'string' && typeof depends[j] !== 'number') {
				sceneResidencyError('region "' + id + '" has an invalid dependency');
			}
		}

		byId.set(id, {
			id: id,
			roots: region.roots,
			bounds: {
				min: [bounds.min[0], bounds.min[1], bounds.min[2]],
				max: [bounds.max[0], bounds.max[1], bounds.max[2]]
			},
			enter: region.enter,
			leave: region.leave,
			depends: depends
		});
	}

	// All dependency references must resolve. Do this after the first pass so
	// that forward references are allowed.
	var normalized = [];
	byId.forEach(function (region, id) {
		for (var i = 0; i < region.depends.length; i++) {
			if (!byId.has(region.depends[i])) {
				sceneResidencyError('region "' + id + '" depends on missing region "' + region.depends[i] + '"');
			}
		}
		normalized.push(region);
	});

	return normalized;
}

function validatePoints(points) {
	if (!Array.isArray(points)) {
		sceneResidencyError('update points must be an array of [x, y, z] triples');
	}
	for (var i = 0; i < points.length; i++) {
		var point = points[i];
		if (!Array.isArray(point) || point.length !== 3) {
			sceneResidencyError('point at index ' + i + ' must be an array of three numbers');
		}
		for (var j = 0; j < 3; j++) {
			if (!isFiniteNumber(point[j])) {
				sceneResidencyError('point at index ' + i + ' has a non-finite coordinate at index ' + j);
			}
		}
	}
}

function squaredDistanceToBounds(bounds, point) {
	var distanceSquared = 0;
	var min = bounds.min;
	var max = bounds.max;
	for (var i = 0; i < 3; i++) {
		var value = point[i];
		var delta;
		if (value < min[i]) {
			delta = min[i] - value;
		} else if (value > max[i]) {
			delta = value - max[i];
		} else {
			delta = 0;
		}
		distanceSquared += delta * delta;
	}
	return distanceSquared;
}

function compareRegionIds(a, b) {
	if (typeof a === 'number' && typeof b === 'number') {
		return a - b;
	}
	var aString = String(a);
	var bString = String(b);
	return aString < bString ? -1 : aString > bString ? 1 : 0;
}

function compareEntityIds(a, b) {
	if (typeof a.id === 'number' && typeof b.id === 'number') {
		return a.id - b.id;
	}
	var aString = String(a.id);
	var bString = String(b.id);
	return aString < bString ? -1 : aString > bString ? 1 : 0;
}

/**
 * Compute the direct activation state for this frame without mutating any
 * controller state. Only directly-activated regions use the hysteresis
 * thresholds; regions pulled in through dependencies never affect this state.
 */
SceneResidency.prototype._computeDirect = function (points) {
	var nextDirect = new Set();

	if (points.length === 0) {
		return nextDirect;
	}

	for (var i = 0; i < this._regions.length; i++) {
		var region = this._regions[i];
		var distanceSquared = squaredDistanceToBounds(region.bounds, points[0]);
		for (var p = 1; p < points.length; p++) {
			var candidate = squaredDistanceToBounds(region.bounds, points[p]);
			if (candidate < distanceSquared) {
				distanceSquared = candidate;
			}
		}

		var wasDirect = region.direct;
		var enterSquared = region.enter * region.enter;
		var leaveSquared = region.leave * region.leave;

		if (!wasDirect) {
			if (distanceSquared <= enterSquared) {
				nextDirect.add(region.id);
			}
		} else if (distanceSquared <= leaveSquared) {
			nextDirect.add(region.id);
		}
	}

	return nextDirect;
};

/**
 * Expand a set of directly active regions through the dependency graph.
 * Reachability (not reference counting) is used so dependency cycles cannot
 * keep each other resident without an external direct path.
 */
SceneResidency.prototype._computeResident = function (directIds) {
	var resident = new Set();
	var stack = [];

	directIds.forEach(function (id) {
		stack.push(id);
	});

	while (stack.length > 0) {
		var id = stack.pop();
		if (resident.has(id)) {
			continue;
		}
		var region = this._regionById.get(id);
		resident.add(id);
		for (var i = 0; i < region.depends.length; i++) {
			stack.push(region.depends[i]);
		}
	}

	return resident;
};

SceneResidency.prototype._validateRoots = function () {
	for (var i = 0; i < this._regions.length; i++) {
		var region = this._regions[i];
		for (var j = 0; j < region.roots.length; j++) {
			var entity = region.roots[j];
			if (!entity || typeof entity !== 'object' || !entity.transformComponent ||
				typeof entity.transformComponent !== 'object') {
				sceneResidencyError('region "' + region.id + '" root at index ' + j + ' no longer has a TransformComponent');
			}
		}
	}
};

SceneResidency.prototype._collectSubtree = function (entity, needed, visited) {
	if (!entity || visited.has(entity)) {
		return;
	}
	visited.add(entity);
	needed.add(entity);

	var transformComponent = entity.transformComponent;
	if (!transformComponent) {
		sceneResidencyError('entity "' + entity.id + '" in a resident subtree has no TransformComponent');
	}

	var children = transformComponent.children;
	for (var i = 0; i < children.length; i++) {
		var child = children[i];
		if (!child || !child.entity) {
			sceneResidencyError('entity "' + entity.id + '" has an invalid child TransformComponent');
		}
		this._collectSubtree(child.entity, needed, visited);
	}
};

/**
 * Collect all entities that must be in the world this frame: the union of the
 * current transform subtrees of the resident regions' roots plus the ancestor
 * chain of each root up to its parentless ancestor. Ancestors are added alone;
 * their other branches are not pulled in.
 */
SceneResidency.prototype._collectNeeded = function (residentIds) {
	var needed = new Set();
	var visited = new Set();
	var self = this;

	residentIds.forEach(function (id) {
		var region = self._regionById.get(id);
		for (var i = 0; i < region.roots.length; i++) {
			var root = region.roots[i];
			self._collectSubtree(root, needed, visited);

			var transformComponent = root.transformComponent;
			while (transformComponent.parent) {
				var parentEntity = transformComponent.parent.entity;
				if (!parentEntity || !parentEntity.transformComponent) {
					sceneResidencyError('a resident root has an ancestor without a valid TransformComponent');
				}
				needed.add(parentEntity);
				transformComponent = transformComponent.parent;
			}
		}
	});

	return needed;
};

/**
 * Update the residency selection and commit the resulting entity membership.
 *
 * @param {Array<Array<number>>} points Observation points for this frame.
 * @returns {{direct: Array, resident: Array, added: Array<Entity>, removed: Array<Entity>}}
 */
SceneResidency.prototype.update = function (points) {
	if (this._disposed) {
		throw new Error('SceneResidency: update() called after dispose()');
	}

	// Validate every input before changing hysteresis, membership or queues.
	validatePoints(points);
	this._validateRoots();

	var nextDirect = this._computeDirect(points);
	var residentIds = this._computeResident(nextDirect);
	var needed = this._collectNeeded(residentIds);

	// Commit hysteresis only after all computations have succeeded.
	for (var i = 0; i < this._regions.length; i++) {
		var region = this._regions[i];
		region.direct = nextDirect.has(region.id);
	}

	var world = this._world;
	var entityManager = world.entityManager;
	var toAdd = [];
	var toRemove = [];

	needed.forEach(function (entity) {
		// External members are fixed: the controller neither adds nor removes them.
		if (!entityManager.containsEntity(entity) && !this._external.has(entity)) {
			toAdd.push(entity);
		}
	}, this);

	this._owned.forEach(function (entity) {
		if (!needed.has(entity)) {
			// Only members the controller still owns and that are actually in
			// the world are reported and removed.
			if (entityManager.containsEntity(entity)) {
				toRemove.push(entity);
			}
			this._owned.delete(entity);
		}
	}, this);

	toAdd.sort(compareEntityIds);
	toRemove.sort(compareEntityIds);

	// Re-entering entities must recompute their world transform from the
	// current hierarchy instead of reusing the matrix they had when leaving.
	for (i = 0; i < toAdd.length; i++) {
		toAdd[i].transformComponent._worldTransformDirty = true;
	}

	for (i = 0; i < toRemove.length; i++) {
		this._owned.delete(toRemove[i]);
	}
	for (i = 0; i < toAdd.length; i++) {
		this._owned.add(toAdd[i]);
	}

	world.commitResidencyMembers(toAdd, toRemove);

	var direct = [];
	nextDirect.forEach(function (id) {
		direct.push(id);
	});
	direct.sort(compareRegionIds);

	var resident = [];
	residentIds.forEach(function (id) {
		resident.push(id);
	});
	resident.sort(compareRegionIds);

	return {
		direct: direct,
		resident: resident,
		added: toAdd.slice(),
		removed: toRemove.slice()
	};
};

/**
 * Release every entity the controller added to the world. External entities
 * and the transform hierarchy are left untouched. Safe to call repeatedly.
 */
SceneResidency.prototype.dispose = function () {
	if (this._disposed) {
		return;
	}

	var toRemove = [];
	this._owned.forEach(function (entity) {
		toRemove.push(entity);
	});
	toRemove.sort(compareEntityIds);

	this._owned.clear();

	this._world.commitResidencyMembers([], toRemove);

	this._world._sceneResidency = null;
	this._disposed = true;
};

SceneResidency.prototype.isActive = function () {
	return !this._disposed;
};

module.exports = SceneResidency;
