import { BYTES_PER_NODE, UINT32_PER_NODE } from '../Constants.js';
import { COUNT, IS_LEAF, LEFT_NODE, OFFSET, RIGHT_NODE } from '../utils/nodeBufferUtils.js';

const REBUILD_MESSAGE =
	'MeshBVH.refitVertices: The geometry topology has changed (vertex count, index presence or ' +
	'length, group ranges, or drawRange). The BVH must be rebuilt to continue.';

// Returns a signature of the geometry state that would invalidate the BVH topology if changed.
function getTopologySignature( geometry ) {

	const groups = geometry.groups;
	let groupsSig = '';
	for ( let i = 0, l = groups.length; i < l; i ++ ) {

		groupsSig += groups[ i ].start + ':' + groups[ i ].count + ',';

	}

	return groupsSig + '|' + geometry.drawRange.start + ':' + geometry.drawRange.count;

}

// Traverses the packed BVH roots and builds the vertex -> leaf node dependency map used for
// sparse refits. Only the index and indirect buffers are read here - never vertex positions -
// so this stays proportional to the triangle count and only runs when topology changes.
function buildVertexRefitState( bvh ) {

	const geometry = bvh.geometry;
	const posAttr = geometry.attributes.position;
	const indexAttr = geometry.index;
	const index = indexAttr ? indexAttr.array : null;
	const indirectBuffer = bvh._indirectBuffer;
	const roots = bvh._roots;
	const vertexCount = posAttr.count;

	// vertex index -> flat list of [ rootIndex, nodeIndex ] pairs referencing every leaf
	// that contains a triangle using the vertex
	const vertexLeafRefs = new Array( vertexCount );
	const rootStates = [];
	let leafCount = 0;

	for ( let rootIndex = 0, l = roots.length; rootIndex < l; rootIndex ++ ) {

		const buffer = roots[ rootIndex ];
		const uint32Array = new Uint32Array( buffer );
		const uint16Array = new Uint16Array( buffer );
		const nodeCount = buffer.byteLength / BYTES_PER_NODE;
		const parents = new Int32Array( nodeCount );
		parents[ 0 ] = - 1;

		// children always follow their parent in the packed layout so parent pointers
		// can be assigned in a single forward pass
		for ( let node = 0; node < nodeCount; node ++ ) {

			const nodeIndex32 = node * UINT32_PER_NODE;
			const nodeIndex16 = nodeIndex32 * 2;

			if ( IS_LEAF( nodeIndex16, uint16Array ) ) {

				leafCount ++;

				const offset = OFFSET( nodeIndex32, uint32Array );
				const count = COUNT( nodeIndex16, uint16Array );
				for ( let i = offset, end = offset + count; i < end; i ++ ) {

					// resolve the BVH-local triangle index to a geometry triangle index
					// before reading the index buffer so the direct index order, the
					// indirect triangle order, and geometry face indices are never mixed
					const tri = indirectBuffer ? indirectBuffer[ i ] : i;
					const tri3 = tri * 3;
					for ( let j = 0; j < 3; j ++ ) {

						const v = index ? index[ tri3 + j ] : tri3 + j;
						let refs = vertexLeafRefs[ v ];
						if ( refs === undefined ) {

							refs = vertexLeafRefs[ v ] = [];

						}

						refs.push( rootIndex, node );

					}

				}

			} else {

				parents[ LEFT_NODE( nodeIndex32 ) / UINT32_PER_NODE ] = node;
				parents[ RIGHT_NODE( nodeIndex32, uint32Array ) / UINT32_PER_NODE ] = node;

			}

		}

		rootStates.push( {
			parents,
			marks: new Uint32Array( nodeCount ),
		} );

	}

	return {
		vertexCount,
		indexAttribute: indexAttr,
		indexVersion: indexAttr ? indexAttr.version : - 1,
		indexCount: indexAttr ? indexAttr.count : 0,
		topologySignature: getTopologySignature( geometry ),
		vertexLeafRefs,
		rootStates,
		leafCount,
		stamp: 0,
	};

}

// Throws an error if the geometry topology changed in a way that requires a BVH rebuild.
function validateTopology( bvh, state ) {

	const geometry = bvh.geometry;
	const indexAttr = geometry.index;

	if (
		geometry.attributes.position.count !== state.vertexCount ||
		( indexAttr !== null ) !== ( state.indexAttribute !== null ) ||
		( indexAttr !== null && indexAttr.count !== state.indexCount ) ||
		getTopologySignature( geometry ) !== state.topologySignature
	) {

		throw new Error( REBUILD_MESSAGE );

	}

}

// Recomputes the bounds of a single leaf node from the current vertex positions. Positions
// are read through the attribute getters so interleaved and normalized attributes are
// supported and only the vertices of triangles in this leaf are touched.
function updateLeafBounds( bvh, nodeIndex32, nodeIndex16, uint32Array, uint16Array, float32Array ) {

	const geometry = bvh.geometry;
	const posAttr = geometry.attributes.position;
	const index = geometry.index ? geometry.index.array : null;
	const indirectBuffer = bvh._indirectBuffer;

	const offset = OFFSET( nodeIndex32, uint32Array );
	const count = COUNT( nodeIndex16, uint16Array );

	let minx = Infinity;
	let miny = Infinity;
	let minz = Infinity;
	let maxx = - Infinity;
	let maxy = - Infinity;
	let maxz = - Infinity;

	for ( let i = offset, l = offset + count; i < l; i ++ ) {

		const tri = indirectBuffer ? indirectBuffer[ i ] : i;
		const tri3 = tri * 3;
		for ( let j = 0; j < 3; j ++ ) {

			let v = tri3 + j;
			if ( index ) {

				v = index[ v ];

			}

			const x = posAttr.getX( v );
			const y = posAttr.getY( v );
			const z = posAttr.getZ( v );

			if ( x < minx ) minx = x;
			if ( x > maxx ) maxx = x;
			if ( y < miny ) miny = y;
			if ( y > maxy ) maxy = y;
			if ( z < minz ) minz = z;
			if ( z > maxz ) maxz = z;

		}

	}

	float32Array[ nodeIndex32 + 0 ] = minx;
	float32Array[ nodeIndex32 + 1 ] = miny;
	float32Array[ nodeIndex32 + 2 ] = minz;
	float32Array[ nodeIndex32 + 3 ] = maxx;
	float32Array[ nodeIndex32 + 4 ] = maxy;
	float32Array[ nodeIndex32 + 5 ] = maxz;

}

// Unions the bounds of both children into the given interior node, matching the
// whole-tree refit behavior.
function updateInteriorBounds( nodeIndex32, uint32Array, float32Array ) {

	const left = LEFT_NODE( nodeIndex32 );
	const right = RIGHT_NODE( nodeIndex32, uint32Array );
	for ( let i = 0; i < 3; i ++ ) {

		const leftMin = float32Array[ left + i ];
		const leftMax = float32Array[ left + i + 3 ];
		const rightMin = float32Array[ right + i ];
		const rightMax = float32Array[ right + i + 3 ];

		float32Array[ nodeIndex32 + i ] = leftMin < rightMin ? leftMin : rightMin;
		float32Array[ nodeIndex32 + i + 3 ] = leftMax > rightMax ? leftMax : rightMax;

	}

}

/**
 * Refits only the BVH leaf nodes (and their ancestors) that contain triangles referencing
 * the given vertex indices. See `MeshBVH.refitVertices` for full documentation.
 *
 * @param {MeshBVH} bvh
 * @param {Iterable<number>} vertexIndices
 * @returns {number} The number of unique leaf nodes that were recomputed.
 */
export function refitVertices( bvh, vertexIndices ) {

	const geometry = bvh.geometry;
	const vertexCount = geometry.attributes.position.count;

	// Consume the iterable exactly once and validate every index before touching any
	// node so a validation failure cannot leave the tree partially updated.
	const seen = new Set();
	const vertices = [];
	for ( const v of vertexIndices ) {

		if ( ! Number.isInteger( v ) || v < 0 || v >= vertexCount ) {

			throw new RangeError(
				`MeshBVH.refitVertices: Vertex index "${ v }" is not a valid vertex index. ` +
				`Expected an integer in the range [ 0, ${ vertexCount - 1 } ].`
			);

		}

		if ( ! seen.has( v ) ) {

			seen.add( v );
			vertices.push( v );

		}

	}

	// Sync the cached vertex -> leaf dependencies with the current geometry state
	let state = bvh._vertexRefitState;
	let resync = false;
	if ( state !== undefined && state !== null ) {

		validateTopology( bvh, state );

		// a replaced index attribute or a bumped index version means the connectivity
		// may have changed even though the topology signature is identical
		const indexAttr = geometry.index;
		resync = indexAttr !== state.indexAttribute ||
			( indexAttr !== null && indexAttr.version !== state.indexVersion );

	}

	if ( ! state || resync ) {

		state = buildVertexRefitState( bvh );
		bvh._vertexRefitState = state;

		if ( resync ) {

			// connectivity changed: rebuild the dependencies and refit the whole tree
			// once before resuming sparse updates
			bvh.refit();
			return state.leafCount;

		}

	}

	// Collect the set of dirty nodes: every leaf referencing a dirty vertex and all of
	// their ancestors, deduplicated via per-call marks.
	state.stamp ++;
	if ( state.stamp >= 0xFFFFFFFF ) {

		// guard against mark overflow on extremely long lived BVHs
		for ( let i = 0, l = state.rootStates.length; i < l; i ++ ) {

			state.rootStates[ i ].marks.fill( 0 );

		}

		state.stamp = 1;

	}

	const stamp = state.stamp;
	const rootStates = state.rootStates;
	const vertexLeafRefs = state.vertexLeafRefs;
	const dirtyNodes = [];
	for ( let i = 0, l = rootStates.length; i < l; i ++ ) {

		dirtyNodes.push( [] );

	}

	for ( let i = 0, l = vertices.length; i < l; i ++ ) {

		const refs = vertexLeafRefs[ vertices[ i ] ];
		if ( refs === undefined ) {

			continue;

		}

		for ( let k = 0, kl = refs.length; k < kl; k += 2 ) {

			const rootIndex = refs[ k ];
			const rootState = rootStates[ rootIndex ];
			const { parents, marks } = rootState;

			let node = refs[ k + 1 ];
			while ( node !== - 1 && marks[ node ] !== stamp ) {

				marks[ node ] = stamp;
				dirtyNodes[ rootIndex ].push( node );
				node = parents[ node ];

			}

		}

	}

	// Update the dirty nodes. Children always have larger packed node indices than their
	// parents so processing nodes in descending index order guarantees children are
	// updated before the parents that read their bounds.
	const roots = bvh._roots;
	let result = 0;
	for ( let rootIndex = 0, l = roots.length; rootIndex < l; rootIndex ++ ) {

		const nodes = dirtyNodes[ rootIndex ];
		if ( nodes.length === 0 ) {

			continue;

		}

		nodes.sort( ( a, b ) => b - a );

		const buffer = roots[ rootIndex ];
		const uint32Array = new Uint32Array( buffer );
		const uint16Array = new Uint16Array( buffer );
		const float32Array = new Float32Array( buffer );

		for ( let i = 0, nl = nodes.length; i < nl; i ++ ) {

			const nodeIndex32 = nodes[ i ] * UINT32_PER_NODE;
			const nodeIndex16 = nodeIndex32 * 2;
			if ( IS_LEAF( nodeIndex16, uint16Array ) ) {

				updateLeafBounds( bvh, nodeIndex32, nodeIndex16, uint32Array, uint16Array, float32Array );
				result ++;

			} else {

				updateInteriorBounds( nodeIndex32, uint32Array, float32Array );

			}

		}

	}

	return result;

}
