import { Box3 } from '../math/Box3.js';
import { Vector3 } from '../math/Vector3.js';

/**
 * A maintainable world-space AABB index over the {@link Mesh} members of an
 * {@link Object3D} subtree, used to accelerate raycast queries.
 *
 * The index is built once from the given root and then kept up to date by
 * listening to the `childadded` / `childremoved` events of every indexed
 * member. Transform changes must be announced with {@link SceneRaycastIndex#markDirty}
 * and vertex changes with {@link SceneRaycastIndex#markGeometryDirty}; pending
 * changes are applied by {@link SceneRaycastIndex#sync}. Queries go through
 * {@link SceneRaycastIndex#intersectRay} which uses the world AABBs only as a
 * broad phase — the actual intersections are still produced by `Mesh.raycast()`.
 *
 * Only plain triangle meshes are supported. Skinned meshes, instanced meshes,
 * batched meshes, morph targets and non-triangle primitives (lines, points,
 * sprites, LODs) are rejected explicitly when they are encountered.
 *
 * The index never modifies the scene graph and does not take ownership of
 * geometries or materials.
 */
class SceneRaycastIndex {

	/**
	 * Constructs a new index for the given root object and indexes its
	 * current subtree.
	 *
	 * @param {Object3D} root - The root of the subtree to index.
	 */
	constructor( root ) {

		if ( root === null || root === undefined || root.isObject3D !== true ) {

			throw new Error( 'SceneRaycastIndex: root must be an Object3D.' );

		}

		this._root = root;

		// membership bookkeeping, keyed by the scene objects themselves
		this._members = new Map(); // Object3D -> { object, parent, children, leaf, transformDirty }
		this._leaves = new Map(); // Mesh -> leaf record
		this._geometryUsers = new Map(); // BufferGeometry -> Set<Mesh>

		// bounding volume hierarchy over the leaf world AABBs
		this._bvhRoot = null;

		// invalidation queues
		this._dirtyRoots = new Set(); // objects whose subtree transforms need a recompute
		this._dirtyGeometries = new Set(); // geometries whose local bounds need a recompute
		this._pendingLeaves = new Set(); // leaves whose world AABB needs a recompute
		this._dirty = false; // any un-synced change

		this._disposed = false;
		this._failure = null; // latched rejection, e.g. unsupported content

		this._stats = { boundsTests: 0, objectRaycasts: 0, leafUpdates: 0, sceneVisits: 0 };

		this._handleChildAdded = ( event ) => {

			if ( this._disposed === true ) return;

			try {

				if ( this._members.has( event.target ) !== true ) return;

				this._addSubtree( event.child );
				this._dirty = true;

			} catch ( error ) {

				// refuse loudly now and keep refusing later instead of
				// silently missing the rejected subtree in future queries
				this._failure = error;
				throw error;

			}

		};

		this._handleChildRemoved = ( event ) => {

			if ( this._disposed === true ) return;

			try {

				if ( this._members.has( event.target ) !== true ) return;

				this._removeSubtree( event.child );
				this._dirty = true;

			} catch ( error ) {

				this._failure = error;
				throw error;

			}

		};

		this._addSubtree( root );
		this._dirty = true;
		this.sync();

	}

	/**
	 * Read-only snapshot of the work counters. The counters are incremented at
	 * the actual operations: `boundsTests` per ray/AABB test, `objectRaycasts`
	 * per delegated `Mesh.raycast()` call, `leafUpdates` per recomputed leaf
	 * world transform/bounds and `sceneVisits` per enumerated scene object or
	 * member record.
	 *
	 * @type {{boundsTests: number, objectRaycasts: number, leafUpdates: number, sceneVisits: number}}
	 */
	get stats() {

		return Object.freeze( { ... this._stats } );

	}

	/**
	 * Resets all work counters to zero.
	 */
	resetStats() {

		this._stats.boundsTests = 0;
		this._stats.objectRaycasts = 0;
		this._stats.leafUpdates = 0;
		this._stats.sceneVisits = 0;

	}

	/**
	 * Marks the given member object as dirty: its world transform and the
	 * world transforms of its current descendants may have changed. Applied
	 * by the next {@link SceneRaycastIndex#sync}.
	 *
	 * @param {Object3D} object - The object whose transform may have changed.
	 */
	markDirty( object ) {

		this._assertUsable();

		if ( object === null || object === undefined || object.isObject3D !== true ) {

			throw new Error( 'SceneRaycastIndex.markDirty: expected an Object3D.' );

		}

		const record = this._members.get( object );

		if ( record === undefined ) {

			throw new Error( 'SceneRaycastIndex.markDirty: object is not a member of this index.' );

		}

		this._markTransformDirty( record );

	}

	/**
	 * Marks the given geometry as dirty: its vertices may have changed, so its
	 * local bounds and the world bounds of all current users are recomputed by
	 * the next {@link SceneRaycastIndex#sync}.
	 *
	 * @param {BufferGeometry} geometry - The geometry whose vertices may have changed.
	 */
	markGeometryDirty( geometry ) {

		this._assertUsable();

		if ( geometry === null || geometry === undefined || geometry.isBufferGeometry !== true ) {

			throw new Error( 'SceneRaycastIndex.markGeometryDirty: expected a BufferGeometry.' );

		}

		this._dirtyGeometries.add( geometry );
		this._dirty = true;

	}

	/**
	 * Applies all pending changes: recomputes the local bounds of dirty
	 * geometries, refreshes the necessary ancestor chains and the world
	 * matrices of dirty subtrees, recomputes the world AABBs of affected
	 * leaves and maintains the bounding volume hierarchy incrementally.
	 *
	 * A sync without any pending change does not traverse the scene and does
	 * not recompute any leaf.
	 */
	sync() {

		this._assertUsable();

		if ( this._dirty === false ) return;

		// 1. recompute the local bounds of dirty geometries and collect the
		//    leaves that reference them, across all branches

		if ( this._dirtyGeometries.size > 0 ) {

			for ( const geometry of this._dirtyGeometries ) {

				this._validateGeometry( geometry );

				geometry.computeBoundingBox();
				geometry.computeBoundingSphere();

				const users = this._geometryUsers.get( geometry );

				if ( users !== undefined ) {

					for ( const mesh of users ) {

						this._stats.sceneVisits ++;

						const leaf = this._leaves.get( mesh );

						if ( leaf !== undefined ) this._pendingLeaves.add( leaf );

					}

				}

			}

			this._dirtyGeometries.clear();

		}

		// 2. recompute the world transforms of dirty subtrees

		if ( this._dirtyRoots.size > 0 ) {

			const roots = [];

			for ( const object of this._dirtyRoots ) {

				const record = this._members.get( object );

				if ( record === undefined || record.transformDirty !== true ) continue;

				// walk the ancestor chain; skip entries that are subsumed
				// by a dirty ancestor (that ancestor's walk covers them)
				let subsumed = false;
				const chain = [];
				let parent = record.parent;

				while ( parent !== null ) {

					this._stats.sceneVisits ++;

					const parentRecord = this._members.get( parent );
					chain.push( parentRecord );

					if ( parentRecord.transformDirty === true ) {

						subsumed = true;
						break;

					}

					parent = parentRecord.parent;

				}

				if ( subsumed === false ) roots.push( { record, chain } );

			}

			this._dirtyRoots.clear();

			for ( let i = 0, il = roots.length; i < il; i ++ ) {

				const { record, chain } = roots[ i ];

				// refresh the necessary ancestor chain, top down
				for ( let j = chain.length - 1; j >= 0; j -- ) {

					this._computeWorldMatrix( chain[ j ] );

				}

				// walk the dirty subtree exactly once
				const stack = [ record ];

				while ( stack.length > 0 ) {

					const current = stack.pop();

					this._stats.sceneVisits ++;

					this._computeWorldMatrix( current );
					current.transformDirty = false;

					if ( current.leaf !== null ) this._pendingLeaves.add( current.leaf );

					for ( const child of current.children ) {

						stack.push( this._members.get( child ) );

					}

				}

			}

		}

		// 3. recompute the world AABBs of affected leaves and maintain the BVH

		if ( this._pendingLeaves.size > 0 ) {

			for ( const leaf of this._pendingLeaves ) {

				this._stats.sceneVisits ++;

				this._updateLeaf( leaf );

			}

			this._pendingLeaves.clear();

		}

		this._dirty = false;

	}

	/**
	 * Appends the intersections of the given raycaster with all indexed meshes
	 * to `target`, sorted by distance, then object id, then face index. The
	 * world AABBs are only used as a broad phase; the intersections themselves
	 * are produced by `Mesh.raycast()`, so layers, near/far, material side and
	 * drawRange behave exactly like the original `Raycaster`.
	 *
	 * The index must be synced before querying; queries never scan the scene
	 * graph to compensate for missed updates.
	 *
	 * @param {Raycaster} raycaster - The raycaster to query with.
	 * @param {Array<Object>} [target=[]] - The array the intersections are appended to.
	 * @return {Array<Object>} The target array.
	 */
	intersectRay( raycaster, target = [] ) {

		this._assertUsable();

		if ( this._dirty === true ) {

			throw new Error( 'SceneRaycastIndex.intersectRay: pending changes, call sync() before querying.' );

		}

		if ( raycaster === null || raycaster === undefined || raycaster.ray === undefined ) {

			throw new Error( 'SceneRaycastIndex.intersectRay: expected a Raycaster.' );

		}

		if ( Array.isArray( target ) !== true ) {

			throw new Error( 'SceneRaycastIndex.intersectRay: target must be an array.' );

		}

		const ray = raycaster.ray;
		const far = raycaster.far;
		const layers = raycaster.layers;

		if ( this._bvhRoot !== null ) {

			const stack = [ this._bvhRoot ];

			while ( stack.length > 0 ) {

				const node = stack.pop();

				this._stats.boundsTests ++;

				if ( _rayIntersectsBox( ray, node.box, far ) !== true ) continue;

				if ( node.leaf !== null ) {

					const mesh = node.leaf.mesh;

					// like the original Raycaster, only the mesh's own layers
					// are tested; ancestors never mask their descendants
					if ( mesh.layers.test( layers ) === true ) {

						this._stats.objectRaycasts ++;

						mesh.raycast( raycaster, target );

					}

				} else {

					stack.push( node.left );
					stack.push( node.right );

				}

			}

		}

		target.sort( _compareIntersections );

		return target;

	}

	/**
	 * Releases all event listeners and object references held by the index.
	 * Safe to call multiple times. Querying or updating the index afterwards
	 * throws an error. Geometries and materials stay owned by the caller and
	 * are not disposed.
	 */
	dispose() {

		if ( this._disposed === true ) return;

		for ( const [ object ] of this._members ) {

			object.removeEventListener( 'childadded', this._handleChildAdded );
			object.removeEventListener( 'childremoved', this._handleChildRemoved );

		}

		this._members.clear();
		this._leaves.clear();
		this._geometryUsers.clear();
		this._dirtyRoots.clear();
		this._dirtyGeometries.clear();
		this._pendingLeaves.clear();

		this._bvhRoot = null;
		this._root = null;
		this._disposed = true;

	}

	// internals

	_assertUsable() {

		if ( this._disposed === true ) {

			throw new Error( 'SceneRaycastIndex: index has been disposed.' );

		}

		if ( this._failure !== null ) {

			throw this._failure;

		}

	}

	_validateObject( object ) {

		if ( object === null || object === undefined || object.isObject3D !== true ) {

			throw new Error( 'SceneRaycastIndex: can only index Object3D instances.' );

		}

		if ( object.isSkinnedMesh === true ) {

			throw new Error( 'SceneRaycastIndex: SkinnedMesh is not supported.' );

		}

		if ( object.isInstancedMesh === true ) {

			throw new Error( 'SceneRaycastIndex: InstancedMesh is not supported.' );

		}

		if ( object.isBatchedMesh === true ) {

			throw new Error( 'SceneRaycastIndex: BatchedMesh is not supported.' );

		}

		if ( object.isLine === true || object.isPoints === true || object.isSprite === true ) {

			throw new Error( `SceneRaycastIndex: non-triangle primitive "${object.type}" is not supported.` );

		}

		if ( object.isLOD === true ) {

			throw new Error( 'SceneRaycastIndex: LOD is not supported.' );

		}

		if ( object.isMesh === true ) {

			if ( object.morphTargetInfluences !== undefined ) {

				throw new Error( 'SceneRaycastIndex: morph targets are not supported.' );

			}

			this._validateGeometry( object.geometry );

		}

	}

	_validateGeometry( geometry ) {

		if ( geometry === null || geometry === undefined || geometry.isBufferGeometry !== true ) {

			throw new Error( 'SceneRaycastIndex: mesh geometry must be a BufferGeometry.' );

		}

		const morphAttributes = geometry.morphAttributes;

		if ( morphAttributes !== undefined && morphAttributes !== null && Object.keys( morphAttributes ).length > 0 ) {

			throw new Error( 'SceneRaycastIndex: morph targets are not supported.' );

		}

	}

	_addSubtree( object ) {

		// collect and validate first so a rejected subtree never leaves
		// partial membership behind
		const objects = [];
		const stack = [ object ];

		while ( stack.length > 0 ) {

			const current = stack.pop();

			this._stats.sceneVisits ++;

			this._validateObject( current );
			objects.push( current );

			const children = current.children;

			for ( let i = 0, il = children.length; i < il; i ++ ) {

				stack.push( children[ i ] );

			}

		}

		for ( let i = 0, il = objects.length; i < il; i ++ ) {

			const current = objects[ i ];

			const record = {
				object: current,
				parent: null,
				children: new Set(),
				leaf: null,
				transformDirty: false
			};

			this._members.set( current, record );

			current.addEventListener( 'childadded', this._handleChildAdded );
			current.addEventListener( 'childremoved', this._handleChildRemoved );

			if ( current.isMesh === true ) {

				const leaf = {
					mesh: current,
					geometry: current.geometry,
					node: new _BVHNode(),
					inTree: false
				};

				leaf.node.leaf = leaf;
				record.leaf = leaf;

				this._leaves.set( current, leaf );
				this._registerGeometryUser( current.geometry, current );
				this._pendingLeaves.add( leaf );

			}

		}

		// link the new records into the membership tree
		for ( let i = 0, il = objects.length; i < il; i ++ ) {

			const current = objects[ i ];
			const record = this._members.get( current );
			const parent = current.parent;

			if ( parent !== null && this._members.has( parent ) === true ) {

				record.parent = parent;
				this._members.get( parent ).children.add( current );

			}

		}

		this._markTransformDirty( this._members.get( object ) );

	}

	_removeSubtree( object ) {

		const record = this._members.get( object );

		if ( record === undefined ) return;

		if ( record.parent !== null ) {

			const parentRecord = this._members.get( record.parent );

			if ( parentRecord !== undefined ) parentRecord.children.delete( object );

		}

		const stack = [ object ];

		while ( stack.length > 0 ) {

			const current = stack.pop();
			const currentRecord = this._members.get( current );

			if ( currentRecord === undefined ) continue;

			this._stats.sceneVisits ++;

			if ( currentRecord.leaf !== null ) {

				const leaf = currentRecord.leaf;

				if ( leaf.inTree === true ) this._removeNode( leaf.node );

				this._unregisterGeometryUser( leaf.geometry, current );
				this._pendingLeaves.delete( leaf );
				this._leaves.delete( current );

			}

			current.removeEventListener( 'childadded', this._handleChildAdded );
			current.removeEventListener( 'childremoved', this._handleChildRemoved );

			for ( const child of currentRecord.children ) {

				stack.push( child );

			}

			this._members.delete( current );

		}

	}

	_registerGeometryUser( geometry, mesh ) {

		let users = this._geometryUsers.get( geometry );

		if ( users === undefined ) {

			users = new Set();
			this._geometryUsers.set( geometry, users );

		}

		users.add( mesh );

	}

	_unregisterGeometryUser( geometry, mesh ) {

		const users = this._geometryUsers.get( geometry );

		if ( users !== undefined ) {

			users.delete( mesh );

			if ( users.size === 0 ) this._geometryUsers.delete( geometry );

		}

	}

	_markTransformDirty( record ) {

		// skip if a dirty ancestor already covers this subtree
		let parent = record.parent;

		while ( parent !== null ) {

			this._stats.sceneVisits ++;

			const parentRecord = this._members.get( parent );

			if ( parentRecord.transformDirty === true ) return;

			parent = parentRecord.parent;

		}

		record.transformDirty = true;
		this._dirtyRoots.add( record.object );
		this._dirty = true;

	}

	_computeWorldMatrix( record ) {

		const object = record.object;

		if ( object.matrixAutoUpdate === true ) object.updateMatrix();

		if ( object.matrixWorldAutoUpdate === true ) {

			const parent = object.parent;

			if ( parent === null ) {

				object.matrixWorld.copy( object.matrix );

			} else {

				object.matrixWorld.multiplyMatrices( parent.matrixWorld, object.matrix );

			}

		}

		object.matrixWorldNeedsUpdate = false;

	}

	_updateLeaf( leaf ) {

		this._stats.leafUpdates ++;

		const mesh = leaf.mesh;
		const geometry = mesh.geometry;

		if ( geometry !== leaf.geometry ) {

			// the mesh was re-pointed at another geometry
			this._validateGeometry( geometry );
			this._unregisterGeometryUser( leaf.geometry, mesh );
			this._registerGeometryUser( geometry, mesh );
			leaf.geometry = geometry;

		}

		if ( geometry.boundingBox === null ) geometry.computeBoundingBox();

		leaf.node.box.copy( geometry.boundingBox ).applyMatrix4( mesh.matrixWorld );

		if ( leaf.node.box.isEmpty() === true ) {

			// empty geometry must not leave ghost hits behind
			if ( leaf.inTree === true ) {

				this._removeNode( leaf.node );
				leaf.inTree = false;

			}

		} else {

			if ( leaf.inTree === true ) this._removeNode( leaf.node );

			this._insertNode( leaf.node );
			leaf.inTree = true;

		}

	}

	_insertNode( node ) {

		node.parent = null;
		node.left = null;
		node.right = null;

		if ( this._bvhRoot === null ) {

			this._bvhRoot = node;
			return;

		}

		// descend towards the sibling that grows the least
		let current = this._bvhRoot;

		while ( current.leaf === null ) {

			const leftGrowth = _unionGrowth( current.left.box, node.box );
			const rightGrowth = _unionGrowth( current.right.box, node.box );

			current = leftGrowth <= rightGrowth ? current.left : current.right;

		}

		const sibling = current;
		const branch = new _BVHNode();

		branch.box.copy( sibling.box ).union( node.box );
		branch.parent = sibling.parent;
		branch.left = sibling;
		branch.right = node;

		sibling.parent = branch;
		node.parent = branch;

		if ( branch.parent === null ) {

			this._bvhRoot = branch;

		} else if ( branch.parent.left === sibling ) {

			branch.parent.left = branch;

		} else {

			branch.parent.right = branch;

		}

		this._refit( branch.parent );

	}

	_removeNode( node ) {

		const parent = node.parent;
		node.parent = null;

		if ( parent === null ) {

			this._bvhRoot = null;
			return;

		}

		const grandParent = parent.parent;
		const sibling = parent.left === node ? parent.right : parent.left;

		sibling.parent = grandParent;

		if ( grandParent === null ) {

			this._bvhRoot = sibling;

		} else {

			if ( grandParent.left === parent ) {

				grandParent.left = sibling;

			} else {

				grandParent.right = sibling;

			}

			this._refit( grandParent );

		}

	}

	_refit( node ) {

		while ( node !== null ) {

			node.box.copy( node.left.box ).union( node.right.box );
			node = node.parent;

		}

	}

}

class _BVHNode {

	constructor() {

		this.box = new Box3();
		this.parent = null;
		this.left = null;
		this.right = null;
		this.leaf = null; // leaf record for leaf nodes, null for branches

	}

}

const _hitPoint = /*@__PURE__*/ new Vector3();
const _unionBox = /*@__PURE__*/ new Box3();

function _surfaceArea( box ) {

	const x = box.max.x - box.min.x;
	const y = box.max.y - box.min.y;
	const z = box.max.z - box.min.z;

	return 2 * ( x * y + y * z + z * x );

}

function _unionGrowth( box, other ) {

	_unionBox.copy( box ).union( other );

	return _surfaceArea( _unionBox ) - _surfaceArea( box );

}

function _rayIntersectsBox( ray, box, far ) {

	const point = ray.intersectBox( box, _hitPoint );

	if ( point === null ) return false;

	// an origin inside the box may produce hits at any distance up to far
	if ( box.containsPoint( ray.origin ) === true ) return true;

	// conservative far culling: the box entry point must be within range
	return ray.origin.distanceToSquared( point ) <= far * far;

}

function _compareIntersections( a, b ) {

	if ( a.distance !== b.distance ) return a.distance - b.distance;

	const aId = a.object.id;
	const bId = b.object.id;

	if ( aId !== bId ) return aId - bId;

	const aFace = a.faceIndex === undefined ? -1 : a.faceIndex;
	const bFace = b.faceIndex === undefined ? -1 : b.faceIndex;

	return aFace - bFace;

}

export { SceneRaycastIndex };
