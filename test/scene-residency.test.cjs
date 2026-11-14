'use strict';

const assert = require('node:assert/strict');

const World = require('../src/goo/entities/World');
const SceneResidency = require('../src/goo/entities/SceneResidency');
const System = require('../src/goo/entities/systems/System');
const TransformSystem = require('../src/goo/entities/systems/TransformSystem');

function makeWorld() {
	const world = new World();
	const transformSystem = new TransformSystem();
	world.setSystem(transformSystem);
	return {world, transformSystem};
}

function box(min, max) {
	return {min, max};
}

function region(id, roots, bounds, enter, leave, depends) {
	return {
		id,
		roots,
		bounds,
		enter,
		leave,
		depends: depends || []
	};
}

function entityIds(entities) {
	return entities.map(function (entity) {
		return entity.id;
	}).sort();
}

// A LocationProbe records the final membership as seen from a real System
// inserted/deleted callback, after the synchronous commit in update().
function watchMembership(world, log) {
	const probe = new System('MembershipProbe', ['TransformComponent']);
	const events = [];

	probe.inserted = function (entity) {
		events.push('in:' + entity.id);
		log.entries.push({entity, kind: 'in', inWorld: world.entityManager.containsEntity(entity)});
	};
	probe.deleted = function (entity) {
		events.push('out:' + entity.id);
		log.entries.push({entity, kind: 'out', inWorld: world.entityManager.containsEntity(entity)});
	};

	world.setSystem(probe);

	return {
		probe,
		events,
		active: function () {
			return probe._activeEntities.slice();
		}
	};
}

function updateAndProcess(world, residency, points) {
	const result = residency.update(points);
	world.process();
	return result;
}

const cases = {};

cases.hysteresis = async function (check) {
	// Two observation points: initially near A, then near B, then back to A.
	const {world} = makeWorld();
	const rootA = world.createEntity('A');
	const rootB = world.createEntity('B');
	rootA.setTranslation(0, 0, 0);
	rootB.setTranslation(100, 0, 0);

	const residency = new SceneResidency(world, [
		region('A', [rootA], box([-1, -1, -1], [1, 1, 1]), 2, 6),
		region('B', [rootB], box([99, -1, -1], [101, 1, 1]), 2, 6)
	]);

	const log = {entries: []};
	const watch = watchMembership(world, log);

	// No points: everything exits.
	const empty = updateAndProcess(world, residency, []);
	check(empty.direct.length === 0, 'no points means no directly active regions');
	check(empty.resident.length === 0, 'no points means no resident regions');
	check(empty.added.length === 0, 'nothing added without points');
	check(empty.removed.length === 0, 'nothing to remove initially');

	// Near A: A enters on the enter threshold.
	const nearA = updateAndProcess(world, residency, [[0, 0, 0]]);
	check(nearA.direct.indexOf('A') !== -1, 'A is directly active when inside its enter radius');
	check(nearA.direct.indexOf('B') === -1, 'B is not directly active when far away');
	check(nearA.resident.indexOf('A') !== -1, 'A is resident');
	check(nearA.added.length === 1 && nearA.added[0] === rootA, 'A root is added');

	// Between enter and leave thresholds: A stays, B stays out.
	const between = updateAndProcess(world, residency, [[4, 0, 0]]);
	check(between.direct.indexOf('A') !== -1, 'A stays directly active between enter and leave');
	check(between.direct.indexOf('B') === -1, 'B does not enter between its enter and leave thresholds');
	check(between.added.length === 0, 'no duplicate insert while A remains resident');
	check(world.entityManager.containsEntity(rootA), 'A root is still in the world');

	// Switch to B: A is beyond leave, B is within enter.
	const nearB = updateAndProcess(world, residency, [[100, 0, 0]]);
	check(nearB.direct.indexOf('A') === -1, 'A exits once beyond its leave threshold');
	check(nearB.direct.indexOf('B') !== -1, 'B enters when within its enter radius');
	check(nearB.added.length === 1 && nearB.added[0] === rootB, 'B root is added');
	check(nearB.removed.length === 1 && nearB.removed[0] === rootA, 'A root is removed');
	check(world.entityManager.containsEntity(rootB), 'B root is now a member');
	check(!world.entityManager.containsEntity(rootA), 'A root is no longer a member');

	// Two observation points at once: nearest point per region decides.
	const both = updateAndProcess(world, residency, [[0, 0, 0], [100, 0, 0]]);
	check(both.direct.indexOf('A') !== -1, 'A is directly active for one observation point');
	check(both.direct.indexOf('B') !== -1, 'B is directly active for the other observation point');
	check(both.resident.indexOf('A') !== -1 && both.resident.indexOf('B') !== -1, 'both regions are resident');
	check(both.added.length === 1 && both.added[0] === rootA, 'A re-enters when a point returns');
	check(both.removed.length === 0, 'B remains resident across the switch');

	// Hysteresis with leave only applies to directly active regions.
	const hysteresisResult = updateAndProcess(world, residency, [[103, 0, 0]]);
	check(hysteresisResult.direct.indexOf('B') !== -1, 'B remains active until leave is exceeded');
	check(hysteresisResult.removed.length === 1 && hysteresisResult.removed[0] === rootA, 'A leaves when no point is near it');

	// Membership callbacks saw the final state: inserts are already members,
	// deletions are already out of the entity manager.
	let insertCallbacks = 0;
	let deleteCallbacks = 0;
	for (const entry of log.entries) {
		if (entry.kind === 'in') {
			insertCallbacks++;
			check(entry.inWorld === true, 'insert callback sees the entity as a final member');
		} else {
			deleteCallbacks++;
			check(entry.inWorld === false, 'delete callback sees the entity already removed');
		}
	}
	check(insertCallbacks > 0, 'real system insert callbacks fired');
	check(deleteCallbacks > 0, 'real system delete callbacks fired');

	// No duplicate notifications for stable members.
	const eventsBefore = watch.events.length;
	const stable = updateAndProcess(world, residency, [[103, 0, 0], [103, 1, 0]]);
	check(stable.added.length === 0 && stable.removed.length === 0, 'stable membership produces no net change');
	check(watch.events.length === eventsBefore, 'stable membership produces no duplicate callbacks');

	// Invalid region data is rejected before any world side effect.
	const entityCount = world.entityManager.size();
	function expectConstructionFailure(regions, label) {
		let error = null;
		try {
			new SceneResidency(world, regions);
		} catch (caught) {
			error = caught;
		}
		check(error instanceof Error, label + ' is rejected');
		check(world.entityManager.size() === entityCount, label + ' has no world side effects');
		check(world._sceneResidency === residency, label + ' does not disturb the existing controller');
	}
	const probeRoot = world.createEntity('probeRoot');
	expectConstructionFailure([
		region('dup', [probeRoot], box([0, 0, 0], [1, 1, 1]), 1, 2),
		region('dup', [probeRoot], box([2, 2, 2], [3, 3, 3]), 1, 2)
	], 'duplicate region id');
	expectConstructionFailure([
		region('badDepends', [probeRoot], box([0, 0, 0], [1, 1, 1]), 1, 2, ['missing'])
	], 'missing dependency reference');
	expectConstructionFailure([
		region('badBounds', [probeRoot], {min: [0, 0, NaN], max: [1, 1, 1]}, 1, 2)
	], 'non-finite bounds');
	expectConstructionFailure([
		region('inverted', [probeRoot], box([0, 0, 2], [1, 1, 1]), 1, 2)
	], 'inverted bounds box');
	expectConstructionFailure([
		region('badThresholds', [probeRoot], box([0, 0, 0], [1, 1, 1]), 3, 2)
	], 'enter greater than leave');

	// Invalid observation points are validated before hysteresis or membership
	// changes: a throwing update must leave the previous frame intact.
	const directBefore = stable.direct.slice();
	const residentBefore = stable.resident.slice();
	const memberCountBefore = world.entityManager.size();
	let pointError = null;
	try {
		residency.update([[Infinity, 0, 0]]);
	} catch (caught) {
		pointError = caught;
	}
	check(pointError instanceof Error, 'non-finite observation point is rejected');
	check(world.entityManager.size() === memberCountBefore, 'membership unchanged after rejected points');
	const afterBadPoint = updateAndProcess(world, residency, [[103, 0, 0], [103, 1, 0]]);
	check(afterBadPoint.direct.join(',') === directBefore.join(','), 'hysteresis unchanged after rejected points');
	check(afterBadPoint.resident.join(',') === residentBefore.join(','), 'resident closure unchanged after rejected points');

	let shapeError = null;
	try {
		residency.update([[1, 2]]);
	} catch (caught) {
		shapeError = caught;
	}
	check(shapeError instanceof Error, 'malformed observation point is rejected');
};

cases.closure = async function (check) {
	const {world} = makeWorld();

	// Region "room" is a deep entity. Its dependencies pull in "structure"
	// which needs "foundation". The two later regions share a dependency
	// "shared" and form a cycle without an external root.
	const outer = world.createEntity('outer');
	const outerSibling = world.createEntity('outerSibling');
	const room = world.createEntity('room');
	const structure = world.createEntity('structure');
	const foundation = world.createEntity('foundation');
	const shared = world.createEntity('shared');
	const cycleA = world.createEntity('cycleA');
	const cycleB = world.createEntity('cycleB');

	outer.setTranslation(10, 0, 0);
	outerSibling.setTranslation(20, 0, 0);
	outer.attachChild(room);
	outer.attachChild(outerSibling);
	room.setTranslation(1, 2, 3);

	const residency = new SceneResidency(world, [
		region('room', [room], box([0, 0, 0], [2, 2, 4]), 1, 3, ['structure']),
		region('structure', [structure], box([50, 50, 50], [51, 51, 51]), 1, 2, ['foundation']),
		region('foundation', [foundation], box([60, 60, 60], [61, 61, 61]), 1, 2, []),
		region('alpha', [cycleA, shared], box([70, 0, 0], [71, 0, 0]), 1, 2, ['beta']),
		region('beta', [cycleB], box([80, 0, 0], [81, 0, 0]), 1, 2, ['alpha'])
	]);

	// Deep root only: its coordinate ancestors come in, their other branches do not.
	const result = updateAndProcess(world, residency, [[1, 1, 1]]);
	check(result.direct.indexOf('room') !== -1, 'room directly active');
	check(result.resident.indexOf('room') !== -1, 'room resident');
	check(result.resident.indexOf('structure') !== -1, 'dependency reached through closure');
	check(result.resident.indexOf('foundation') !== -1, 'transitive dependency reached through closure');
	check(result.resident.indexOf('alpha') === -1, 'cycle members without an external root stay out');

	check(world.entityManager.containsEntity(room), 'deep room is a member');
	check(world.entityManager.containsEntity(outer), 'ancestor chain up to parentless root is included');
	check(!world.entityManager.containsEntity(outerSibling), 'non-resident sibling branch is not pulled in');
	check(world.entityManager.containsEntity(structure), 'dependency root is a member');
	check(world.entityManager.containsEntity(foundation), 'transitive dependency root is a member');
	check(!world.entityManager.containsEntity(shared), 'shared dependency with no external root stays out');

	const addedIds = entityIds(result.added);
	check(addedIds.length === 4, 'exactly the room, its ancestor, and the two dependency roots are added');
	check(addedIds.indexOf(room.id) !== -1, 'room itself is in the added set');
	check(addedIds.indexOf(outer.id) !== -1, 'ancestor is in the added set');
	check(addedIds.indexOf(structure.id) !== -1, 'structure root is in the added set');
	check(addedIds.indexOf(foundation.id) !== -1, 'foundation root is in the added set');
	check(addedIds.indexOf(outerSibling.id) === -1, 'non-resident sibling is not in the added set');

	// Break the dependency chain: room stays, foundation leaves.
	const noStructureDirect = updateAndProcess(world, residency, [[50, 50, 50]]);
	check(noStructureDirect.direct.indexOf('room') === -1, 'room exits when no point is near it');
	check(noStructureDirect.direct.indexOf('structure') !== -1, 'structure directly active');
	check(noStructureDirect.resident.indexOf('foundation') !== -1, 'foundation still resident through structure');
	check(world.entityManager.containsEntity(foundation), 'foundation remains while required');

	// Bring structure and alpha into one frame; alpha's cycle must not keep beta
	// alive once alpha no longer has an external path.
	const bothCycles = updateAndProcess(world, residency, [[50, 50, 50], [70, 0, 0]]);
	check(bothCycles.direct.indexOf('alpha') !== -1, 'alpha directly active');
	check(bothCycles.direct.indexOf('beta') === -1, 'beta is not directly active');
	check(bothCycles.resident.indexOf('beta') !== -1, 'beta is resident through the alpha dependency');

	// Remove alpha's direct point: the cycle cannot sustain itself.
	const lostRoot = updateAndProcess(world, residency, [[50, 50, 50], [500, 500, 500]]);
	check(lostRoot.direct.indexOf('alpha') === -1, 'alpha no longer directly active');
	check(lostRoot.resident.indexOf('alpha') === -1, 'cycle member alpha exits');
	check(lostRoot.resident.indexOf('beta') === -1, 'cycle member beta exits with alpha');
	check(!world.entityManager.containsEntity(cycleA), 'alpha root removed from world');
	check(!world.entityManager.containsEntity(cycleB), 'beta root removed from world');
	check(!world.entityManager.containsEntity(shared), 'shared root removed from world');

	// A diamond dependency still holds while another direct path exists.
	// alpha and gene both depend on shared; alpha losing its point must not
	// remove shared while gene is directly active.
	// Only one controller may own a World at a time, so release the first one.
	residency.dispose();
	const gene = world.createEntity('gene');
	const residency2 = new SceneResidency(world, [
		region('alpha2', [cycleA], box([70, 0, 0], [71, 0, 0]), 1, 2, ['shared2']),
		region('gene', [gene], box([90, 0, 0], [91, 0, 0]), 1, 2, ['shared2']),
		region('shared2', [shared], box([95, 0, 0], [96, 0, 0]), 1, 2, [])
	]);
	const diamond = updateAndProcess(world, residency2, [[70, 0, 0], [90, 0, 0]]);
	check(diamond.resident.indexOf('alpha2') !== -1, 'diamond alpha member is resident');
	check(diamond.resident.indexOf('gene') !== -1, 'diamond gene member is resident');
	check(diamond.resident.indexOf('shared2') !== -1, 'shared dependency is resident');
	check(world.entityManager.containsEntity(shared), 'shared root is a member through both paths');

	const diamondOnePath = updateAndProcess(world, residency2, [[90, 0, 0]]);
	check(diamondOnePath.resident.indexOf('alpha2') === -1, 'alpha2 leaves when its point leaves');
	check(diamondOnePath.resident.indexOf('shared2') !== -1, 'shared stays while gene directly needs it');
	check(world.entityManager.containsEntity(shared), 'shared remains a member as long as one direct path needs it');

	residency2.dispose();
	check(!world.entityManager.containsEntity(shared), 'dispose releases the residency-owned shared root');

	// Shared subtree migration: the real hierarchy changes between updates.
	// A child that starts under one region root is moved under another while
	// both regions stay resident. Membership is recomputed from the hierarchy
	// as it actually is this frame, and identity dedup means no churn.
	const ownerA = world.createEntity('ownerA');
	const ownerB = world.createEntity('ownerB');
	const migratingChild = world.createEntity('migratingChild');
	ownerA.setTranslation(150, 0, 0);
	ownerB.setTranslation(160, 0, 0);
	ownerA.attachChild(migratingChild);
	migratingChild.setTranslation(3, 0, 0);

	const migration = new SceneResidency(world, [
		region('ownerA', [ownerA], box([150, -1, -1], [151, 1, 1]), 1, 2, []),
		region('ownerB', [ownerB], box([160, -1, -1], [161, 1, 1]), 1, 2, [])
	]);

	const bothOwners = updateAndProcess(world, migration, [[150, 0, 0], [160, 0, 0]]);
	check(bothOwners.resident.indexOf('ownerA') !== -1 && bothOwners.resident.indexOf('ownerB') !== -1,
		'both shared owners are resident');
	check(world.entityManager.containsEntity(migratingChild), 'child is a member through ownerA subtree');

	ownerB.attachChild(migratingChild);
	const afterMigration = updateAndProcess(world, migration, [[150, 0, 0], [160, 0, 0]]);
	check(afterMigration.added.length === 0 && afterMigration.removed.length === 0,
		'migrated child stays resident: no duplicate add/remove events');
	check(world.entityManager.containsEntity(migratingChild), 'migrated child is still a member');
	check(migratingChild.transformComponent.parent === ownerB.transformComponent,
		'migrated child now really belongs to ownerB');

	// Leaving the current owner's subtree must remove it even though the old
	// owner root is still resident.
	const onlyOwnerA = updateAndProcess(world, migration, [[150, 0, 0]]);
	check(onlyOwnerA.resident.indexOf('ownerB') === -1, 'ownerB leaves the resident closure');
	check(world.entityManager.containsEntity(ownerA), 'ownerA stays resident');
	check(!world.entityManager.containsEntity(migratingChild), 'migrated child follows its current owner subtree out');
	check(!world.entityManager.containsEntity(ownerB), 'ownerB root leaves');

	migration.dispose();
	check(!world.entityManager.containsEntity(ownerA), 'dispose releases ownerA');
	check(!world.entityManager.containsEntity(migratingChild), 'dispose leaves migrated child out');
};

cases.lifecycle = async function (check) {
	const {world, transformSystem} = makeWorld();

	// External fixed member already in the world before the controller exists.
	const external = world.createEntity('external');
	world.addEntity(external);
	const externalAncestor = world.createEntity('externalAncestor');
	world.addEntity(externalAncestor);

	// Region with an ancestor and a non-resident sibling branch.
	const anchor = world.createEntity('anchor');
	const nonResident = world.createEntity('nonResident');
	const residentRoot = world.createEntity('residentRoot');
	const residentChild = world.createEntity('residentChild');

	anchor.setTranslation(4, 0, 0);
	anchor.attachChild(nonResident);
	anchor.attachChild(residentRoot);
	nonResident.setTranslation(0, 7, 0);
	residentRoot.setTranslation(1, 0, 0);
	residentRoot.attachChild(residentChild);
	residentChild.setTranslation(0, 2, 0);

	let nonResidentUpdates = 0;
	const TransformComponent = require('../src/goo/entities/components/TransformComponent');
	const originalUpdateWorldTransform = TransformComponent.prototype.updateWorldTransform;
	TransformComponent.prototype.updateWorldTransform = function () {
		if (this.entity === nonResident) {
			nonResidentUpdates++;
		}
		return originalUpdateWorldTransform.apply(this, arguments);
	};

	// Real system callback that inspects the entity manager for the final members.
	const log = {entries: []};
	const watch = watchMembership(world, log);

	const residency = new SceneResidency(world, [
		region('room', [residentRoot], box([3, -1, -1], [6, 1, 1]), 1, 2, [])
	]);

	check(world._sceneResidency === residency, 'controller is registered on the world');

	// Re-entering from a fresh world: the root's ancestor is included, but not
	// the ancestor's other branch.
	const first = updateAndProcess(world, residency, [[4, 0, 0]]);
	check(first.direct.indexOf('room') !== -1, 'room directly active');
	check(world.entityManager.containsEntity(residentRoot), 'resident root added to world');
	check(world.entityManager.containsEntity(residentChild), 'resident subtree child added to world');
	check(world.entityManager.containsEntity(anchor), 'ancestor added to world');
	check(!world.entityManager.containsEntity(nonResident), 'non-resident sibling branch not added');
	check(world.entityManager.containsEntity(external), 'external member kept');
	check(world.entityManager.containsEntity(externalAncestor), 'external member kept');

	// Final membership from the real System callbacks. The probe was set up
	// before the controller, so the two external entities were inserted when
	// the system was added; only the three residency members follow.
	const residencyInsertEvents = watch.events.filter(function (event) {
		return event.indexOf('in:') === 0;
	}).slice(2);
	check(residencyInsertEvents.length === 3, 'three membership insert notifications, one per real member');
	check(watch.active().indexOf(residentRoot) !== -1, 'resident root tracked by real system');
	check(watch.active().indexOf(anchor) !== -1, 'ancestor tracked by real system');
	check(watch.active().indexOf(nonResident) === -1, 'non-resident branch not tracked by real system');

	// TransformSystem updates: non-resident branch must get zero updates.
	world.process();
	check(nonResidentUpdates === 0, 'non-resident branch updateWorldTransform calls stay at zero');
	check(residentChild.transformComponent.worldTransform.translation.x === 5, 'world x composes ancestor (4) and parent (1)');
	check(residentChild.transformComponent.worldTransform.translation.y === 2, 'world y from the child local transform');
	check(anchor.transformComponent.worldTransform.translation.x === 4, 'ancestor world transform updated');

	// Move the ancestor while the region is out, then re-enter and verify
	// the world value is recomputed from the current ancestor, not the cached
	// world matrix from when the region left.
	const left = updateAndProcess(world, residency, [[200, 200, 200]]);
	check(left.removed.length === 3, 'leaving removes all own resident members');
	check(!world.entityManager.containsEntity(residentRoot), 'resident root removed from world');
	check(!world.entityManager.containsEntity(residentChild), 'resident child removed from world');
	check(!world.entityManager.containsEntity(anchor), 'own ancestor removed from world');
	check(!world.entityManager.containsEntity(nonResident), 'non-resident sibling branch is never a member');

	// The hierarchy is intact: no detach, no cleared children, local transform unchanged.
	check(residentRoot.transformComponent.parent === anchor.transformComponent, 'parent relation preserved after exit');
	check(anchor.transformComponent.children.length === 2, 'ancestor children array preserved after exit');
	check(residentRoot.transformComponent.children.length === 1, 'resident root children array preserved after exit');
	check(residentChild.transformComponent.parent === residentRoot.transformComponent, 'child parent relation preserved');
	check(residentChild.transformComponent.transform.translation.y === 2, 'child local transform unchanged');

	anchor.setTranslation(30, 0, 0);
	const reentered = updateAndProcess(world, residency, [[4, 0, 0], [30, 0, 0]]);
	check(reentered.added.length === 3, 're-entry adds the same three members back');
	check(nonResidentUpdates === 0, 'non-resident branch still receives zero updates after re-entry');
	check(residentChild.transformComponent.worldTransform.translation.x === 31, 're-entry recomputes from the moved current ancestor');
	check(residentChild.transformComponent.transform.translation.y === 2, 'local transform is not overwritten on re-entry');

	// Membership callbacks during the whole lifecycle: every inserted entity
	// was already in the final membership when its callback ran, and removals
	// likewise.
	check(log.entries.length > 0, 'cycle membership callbacks recorded');
	const residencyEvents = watch.events.slice(2);
	check(residencyEvents.length === 9, 'three inserts, three deletes and three re-inserts, no duplicates');
	check(residencyEvents.filter(function (event) { return event.indexOf('in:') === 0; }).length === 6,
		'each resident member is inserted exactly twice (initial entry and re-entry)');
	check(residencyEvents.filter(function (event) { return event.indexOf('out:') === 0; }).length === 3,
		'each resident member is deleted exactly once');

	// dispose releases owned residents but keeps externals and the tree.
	residency.dispose();
	check(!world.entityManager.containsEntity(residentRoot), 'dispose removes owned resident root');
	check(!world.entityManager.containsEntity(residentChild), 'dispose removes owned resident child');
	check(!world.entityManager.containsEntity(anchor), 'dispose removes owned ancestor');
	check(world.entityManager.containsEntity(external), 'dispose keeps external fixed member');
	check(world.entityManager.containsEntity(externalAncestor), 'dispose keeps external fixed member');
	check(!world.entityManager.containsEntity(nonResident), 'dispose does not invent a non-resident branch member');
	check(anchor.transformComponent.children.length === 2, 'dispose leaves the tree complete');
	check(residentRoot.transformComponent.parent === anchor.transformComponent, 'dispose leaves parent relation complete');

	// TransformSystem falls back to the legacy path after dispose: an
	// ordinary world member is updated again through the regular traversal.
	external.setTranslation(7, 0, 0);
	world.process();
	check(external.transformComponent.worldTransform.translation.x === 7, 'legacy transform processing updates world members after dispose');
	check(transformSystem.numUpdates > 0, 'legacy transform processing counts updates after dispose');

	const secondDispose = residency.dispose();
	check(secondDispose === undefined, 'repeated dispose has no side effects');

	// Any later update must fail clearly.
	let updateError = null;
	try {
		residency.update([]);
	} catch (error) {
		updateError = error;
	}
	check(updateError instanceof Error, 'update after dispose throws an Error');
	check(/dispose/i.test(updateError && updateError.message), 'update after dispose error message is explicit');
	check(world._sceneResidency === null, 'dispose clears the world controller reference');

	TransformComponent.prototype.updateWorldTransform = originalUpdateWorldTransform;

	// A new controller can be created on the freed world.
	const replacement = new SceneResidency(world, [
		region('other', [residentChild], box([20, 20, 20], [21, 21, 21]), 1, 2, [])
	]);
	const otherResult = updateAndProcess(world, replacement, [[20, 20, 20]]);
	check(otherResult.added.length > 0, 'replacement controller can add its own roots');
	check(world.entityManager.containsEntity(residentChild), 'replacement controller owns the deep child');
	check(world.entityManager.containsEntity(anchor), 'replacement controller adds the full ancestor chain');
	check(world.entityManager.containsEntity(residentRoot), 'replacement controller adds intermediate ancestors too');
	check(!world.entityManager.containsEntity(nonResident), 'replacement controller still skips the ancestor sibling branch');
};

module.exports = {cases};
