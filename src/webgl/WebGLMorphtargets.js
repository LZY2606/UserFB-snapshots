import { Attribute } from '../resources/geometries/Attribute.js';
import { Buffer } from '../resources/geometries/Buffer.js';
import { BUFFER_USAGE } from '../const.js';

// The shader interface is fixed (see shaderChunk/common_vert.glsl and
// morphtarget_pars_vert.glsl): 8 morphTarget position slots, or 4
// morphTarget / morphNormal pairs when morph normals are present.
const MAX_SLOTS = 8;
const MAX_SLOTS_WITH_NORMALS = 4;

const _influences = new Float32Array(MAX_SLOTS);
const _influencesWithNormals = _influences.subarray(0, MAX_SLOTS_WITH_NORMALS);

// Per-geometry submit state, shared by every mesh rendered with the geometry.
// Temporary composite buffers are owned here and are never written into the
// source morphAttributes, so shared source assets stay untouched.
const _states = new WeakMap();

function getState(geometry) {
	let state = _states.get(geometry);

	if (state === undefined) {
		state = {
			// slot attributes currently added to the geometry
			boundPositionSlots: 0,
			boundNormalSlots: 0,
			// scratch list of active target indices
			active: [],
			// temporary composite attributes (overflow path only)
			tempPositions: null,
			tempNormals: null,
			tempSlots: 0,
			tempVertexCount: 0,
			// describes the data the temporary buffers were composited from
			signature: null,
			// last WebGLBuffers cache seen, used to release temporary buffers
			buffers: null,
			disposeListener: null
		};
		_states.set(geometry, state);
	}

	return state;
}

function ensureDisposeListener(state, geometry) {
	if (state.disposeListener !== null) return;

	const listener = () => {
		releaseTempSlots(state, state.buffers);
		state.disposeListener = null;
	};

	state.disposeListener = listener;
	geometry.addEventListener('dispose', listener);
}

function releaseTempSlots(state, buffers) {
	if (state.tempPositions === null) return;

	if (buffers) {
		for (let i = 0; i < state.tempPositions.length; i++) {
			buffers.removeBuffer(state.tempPositions[i].buffer);
		}
		if (state.tempNormals !== null) {
			for (let i = 0; i < state.tempNormals.length; i++) {
				buffers.removeBuffer(state.tempNormals[i].buffer);
			}
		}
	}

	state.tempPositions = null;
	state.tempNormals = null;
	state.tempSlots = 0;
	state.tempVertexCount = 0;
	state.signature = null;
}

function createTempAttributes(vertexCount, slots) {
	const attributes = [];

	for (let i = 0; i < slots; i++) {
		const buffer = new Buffer(new Float32Array(vertexCount * 3), 3);
		buffer.usage = BUFFER_USAGE.DYNAMIC_DRAW;
		attributes.push(new Attribute(buffer, 3));
	}

	return attributes;
}

function ensureTempSlots(state, geometry, buffers, slots, hasNormals, vertexCount) {
	if (state.tempPositions !== null
		&& state.tempSlots === slots
		&& state.tempVertexCount === vertexCount
		&& (state.tempNormals !== null) === hasNormals) {
		return;
	}

	releaseTempSlots(state, buffers);

	state.tempPositions = createTempAttributes(vertexCount, slots);
	state.tempNormals = hasNormals ? createTempAttributes(vertexCount, slots) : null;
	state.tempSlots = slots;
	state.tempVertexCount = vertexCount;
	state.signature = null;

	ensureDisposeListener(state, geometry);
}

// Accumulate the weighted deltas of the active targets into the temporary
// slot buffers. Active target k is folded into slot (k % slots), so every
// slot keeps a partial sum and slot * influence 1 restores the full sum.
function compositeTempSlots(state, objectInfluences, morphTargets, morphNormals, active, slots, hasNormals, vertexCount) {
	for (let i = 0; i < slots; i++) {
		state.tempPositions[i].buffer.array.fill(0);
		if (hasNormals) state.tempNormals[i].buffer.array.fill(0);
	}

	for (let k = 0; k < active.length; k++) {
		const index = active[k];
		const weight = objectInfluences[index];
		const slot = k % slots;

		accumulate(state.tempPositions[slot].buffer.array, morphTargets[index], weight, vertexCount);

		if (hasNormals && morphNormals[index]) {
			accumulate(state.tempNormals[slot].buffer.array, morphNormals[index], weight, vertexCount);
		}
	}

	for (let i = 0; i < slots; i++) {
		state.tempPositions[i].buffer.version++;
		if (hasNormals) state.tempNormals[i].buffer.version++;
	}
}

// Only the 3 components addressed by Buffer.stride and Attribute.offset are
// read; other interleaved components never enter the deformation.
function accumulate(dst, attribute, weight, vertexCount) {
	const src = attribute.buffer.array;
	const stride = attribute.buffer.stride;
	const offset = attribute.offset;

	let srcIndex = offset;
	let dstIndex = 0;

	for (let v = 0; v < vertexCount; v++) {
		dst[dstIndex] += src[srcIndex] * weight;
		dst[dstIndex + 1] += src[srcIndex + 1] * weight;
		dst[dstIndex + 2] += src[srcIndex + 2] * weight;
		srcIndex += stride;
		dstIndex += 3;
	}
}

// The composite is only valid for the exact combination of weights, target
// attributes and buffer versions it was built from. Returns true when the
// temporary buffers have to be recomposited.
function updateSignature(state, objectInfluences, morphTargets, morphNormals, active, slots, hasNormals, vertexCount) {
	let signature = state.signature;

	if (signature !== null
		&& signature.hasNormals === hasNormals
		&& signature.vertexCount === vertexCount
		&& signature.active.length === active.length) {
		let same = true;

		for (let k = 0; k < active.length; k++) {
			const index = active[k];

			if (signature.active[k] !== index || signature.weights[k] !== objectInfluences[index]) {
				same = false;
				break;
			}

			if (!matchAttributeRecord(signature.positions[k], morphTargets[index])) {
				same = false;
				break;
			}

			if (hasNormals && !matchAttributeRecord(signature.normals[k], morphNormals[index])) {
				same = false;
				break;
			}
		}

		if (same) return false;
	}

	signature = {
		hasNormals: hasNormals,
		vertexCount: vertexCount,
		active: active.slice(),
		weights: [],
		positions: [],
		normals: []
	};

	for (let k = 0; k < active.length; k++) {
		const index = active[k];
		signature.weights.push(objectInfluences[index]);
		signature.positions.push(recordAttribute(morphTargets[index]));
		signature.normals.push(hasNormals ? recordAttribute(morphNormals[index]) : null);
	}

	state.signature = signature;

	return true;
}

function recordAttribute(attribute) {
	if (!attribute) return null;
	return {
		attribute: attribute,
		version: attribute.buffer.version,
		offset: attribute.offset
	};
}

function matchAttributeRecord(record, attribute) {
	if (record === null) return !attribute;
	if (!attribute) return false;
	return record.attribute === attribute
		&& record.version === attribute.buffer.version
		&& record.offset === attribute.offset;
}

/**
 * Prepare morph target slot attributes and influences for the next draw.
 *
 * Weights are used as-is: any finite positive or negative value counts as
 * active and is never truncated, renormalized or dropped. While the active
 * targets fit the shader slots, the original target Attributes are bound
 * directly (no per-vertex copies). When the active targets overflow the
 * slots, their weighted deltas are composited into reusable temporary
 * buffers and every slot is submitted with influence 1, so the unmodified
 * shader formula still accumulates all contributions.
 * @param {Mesh} object - The mesh being rendered.
 * @param {Geometry} geometry - The geometry being rendered.
 * @param {WebGLBuffers} buffers - The renderer buffer cache, used to upload
 *        and release temporary composite buffers through the usual chain.
 * @param {WebGLRenderingContext} gl - The rendering context.
 * @returns {Float32Array} The morphTargetInfluences value to upload
 *        (length 8, or length 4 when morph normals are present).
 */
function submitMorphTargets(object, geometry, buffers, gl) {
	const objectInfluences = object.morphTargetInfluences;
	const morphTargets = geometry.morphAttributes.position;
	const morphNormals = geometry.morphAttributes.normal;
	const hasNormals = !!morphNormals;
	const slots = hasNormals ? MAX_SLOTS_WITH_NORMALS : MAX_SLOTS;

	const state = getState(geometry);

	state.buffers = buffers;

	// Remove the slot attributes bound for the previous submission.

	for (let i = 0; i < state.boundPositionSlots; i++) {
		geometry.removeAttribute('morphTarget' + i);
	}
	for (let i = 0; i < state.boundNormalSlots; i++) {
		geometry.removeAttribute('morphNormal' + i);
	}
	state.boundPositionSlots = 0;
	state.boundNormalSlots = 0;

	// Collect the active targets: any non-zero weight, negative included.

	const active = state.active;
	active.length = 0;

	const targetCount = morphTargets ? morphTargets.length : 0;

	for (let i = 0; i < targetCount; i++) {
		if (objectInfluences[i]) active.push(i);
	}

	let usedSlots = 0;

	if (active.length <= slots) {
		// Fast path: bind the original target attributes directly.

		for (let k = 0; k < active.length; k++) {
			const index = active[k];

			geometry.addAttribute('morphTarget' + k, morphTargets[index]);
			if (hasNormals && morphNormals[index]) {
				geometry.addAttribute('morphNormal' + k, morphNormals[index]);
			}

			_influences[k] = objectInfluences[index];
		}

		usedSlots = active.length;
		state.boundPositionSlots = active.length;
		state.boundNormalSlots = hasNormals ? active.length : 0;
	} else {
		// Overflow path: composite all active targets into temporary slots.

		const positionAttribute = geometry.getAttribute('a_Position');
		const vertexCount = positionAttribute ? positionAttribute.buffer.count : morphTargets[active[0]].buffer.count;

		ensureTempSlots(state, geometry, buffers, slots, hasNormals, vertexCount);

		if (updateSignature(state, objectInfluences, morphTargets, morphNormals, active, slots, hasNormals, vertexCount)) {
			compositeTempSlots(state, objectInfluences, morphTargets, morphNormals, active, slots, hasNormals, vertexCount);
		}

		for (let k = 0; k < slots; k++) {
			geometry.addAttribute('morphTarget' + k, state.tempPositions[k]);
			if (hasNormals) {
				geometry.addAttribute('morphNormal' + k, state.tempNormals[k]);
			}

			_influences[k] = 1;

			// Upload through the regular WebGLBuffers chain; this is a no-op
			// unless the composite was recomputed above.
			if (buffers) {
				buffers.setBuffer(state.tempPositions[k].buffer, gl.ARRAY_BUFFER);
				if (hasNormals) buffers.setBuffer(state.tempNormals[k].buffer, gl.ARRAY_BUFFER);
			}
		}

		usedSlots = slots;
		state.boundPositionSlots = slots;
		state.boundNormalSlots = hasNormals ? slots : 0;
	}

	for (let k = usedSlots; k < MAX_SLOTS; k++) {
		_influences[k] = 0;
	}

	return hasNormals ? _influencesWithNormals : _influences;
}

export { submitMorphTargets };
