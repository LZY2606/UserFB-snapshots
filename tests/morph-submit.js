import assert from 'node:assert/strict';

import { WebGLRenderer } from '../src/webgl/WebGLRenderer.js';
import { WebGLBuffers } from '../src/webgl/WebGLBuffers.js';
import { WebGLGeometries } from '../src/webgl/WebGLGeometries.js';
import { WebGLVertexArrayBindings } from '../src/webgl/WebGLVertexArrayBindings.js';
import { Mesh } from '../src/scenes/Mesh.js';
import { Geometry } from '../src/resources/geometries/Geometry.js';
import { Attribute } from '../src/resources/geometries/Attribute.js';
import { Buffer } from '../src/resources/geometries/Buffer.js';
import { Vector3 } from '../src/math/Vector3.js';

const EPS = 1e-6;

// ---------------------------------------------------------------------------
// Lightweight device double: records uniforms, attribute bindings and buffer
// uploads while the real WebGLBuffers / WebGLGeometries /
// WebGLVertexArrayBindings chain runs against it.
// ---------------------------------------------------------------------------

function createDeviceDouble() {
	const rec = {
		createdBuffers: [],
		deletedBuffers: [],
		uploads: [],
		pointerCalls: [],
		enabledLocations: []
	};

	let boundArrayBuffer = null;

	const gl = {
		ARRAY_BUFFER: 0x8892,
		ELEMENT_ARRAY_BUFFER: 0x8893,
		FLOAT: 0x1406,
		INT: 0x1404,
		UNSIGNED_INT: 0x1405,

		createBuffer() {
			const buffer = { id: rec.createdBuffers.length, data: [], uploadCount: 0 };
			rec.createdBuffers.push(buffer);
			return buffer;
		},
		deleteBuffer(buffer) {
			rec.deletedBuffers.push(buffer);
		},
		bindBuffer(target, buffer) {
			if (target === gl.ARRAY_BUFFER) boundArrayBuffer = buffer;
		},
		bufferData(target, data, usage) {
			const buffer = boundArrayBuffer;
			buffer.data = Array.from(data);
			buffer.uploadCount++;
			rec.uploads.push({ buffer: buffer, kind: 'data' });
		},
		bufferSubData(target, dstOffset, data, srcOffset, srcLength) {
			const buffer = boundArrayBuffer;
			const view = srcOffset === undefined ? data : data.subarray(srcOffset, srcOffset + srcLength);
			const start = dstOffset / data.BYTES_PER_ELEMENT;
			for (let i = 0; i < view.length; i++) buffer.data[start + i] = view[i];
			buffer.uploadCount++;
			rec.uploads.push({ buffer: buffer, kind: 'sub' });
		},
		createVertexArray() {
			return {};
		},
		bindVertexArray() {},
		deleteVertexArray() {},
		enableVertexAttribArray(location) {
			rec.enabledLocations.push(location);
		},
		vertexAttribPointer(location, size, type, normalized, stride, offset) {
			rec.pointerCalls.push({ location: location, size: size, stride: stride, offset: offset, buffer: boundArrayBuffer });
		}
	};

	return { gl: gl, rec: rec };
}

function createHarness() {
	const device = createDeviceDouble();
	const renderer = new WebGLRenderer();
	const capabilities = { version: 2, getExtension: () => null };

	renderer.context = device.gl;
	renderer.capabilities = capabilities;
	renderer._buffers = new WebGLBuffers('_buffers', device.gl, capabilities, renderer.gpuMemory);
	renderer._vertexArrayBindings = new WebGLVertexArrayBindings('_vab', device.gl, capabilities, renderer._buffers);
	renderer._geometries = new WebGLGeometries('_geometries', device.gl, renderer._buffers, renderer._vertexArrayBindings);

	return { renderer: renderer, gl: device.gl, rec: device.rec, pass: 0 };
}

// Program double mirroring the shader's morph slot layout.
function createProgram(hasNormals) {
	const attributes = {
		a_Position: { count: 3, location: 0, locationSize: 1, format: 0x1406 },
		a_Normal: { count: 3, location: 1, locationSize: 1, format: 0x1406 }
	};

	let location = 2;

	const positionSlots = hasNormals ? 4 : 8;

	for (let i = 0; i < positionSlots; i++) {
		attributes['morphTarget' + i] = { count: 3, location: location++, locationSize: 1, format: 0x1406 };
	}

	if (hasNormals) {
		for (let i = 0; i < 4; i++) {
			attributes['morphNormal' + i] = { count: 3, location: location++, locationSize: 1, format: 0x1406 };
		}
	}

	const program = {
		uniforms: {},
		getAttributes() {
			return attributes;
		},
		getUniforms() {
			return {
				set(name, value) {
					program.uniforms[name] = Array.from(value);
				}
			};
		}
	};

	return program;
}

// Drives the real pre-draw morph path in the same order as
// WebGLRenderer.renderRenderableItem.
function draw(h, mesh, program, newPass) {
	if (newPass !== false) h.pass++;

	const geometry = mesh.geometry;

	h.rec.pointerCalls.length = 0;
	h.rec.enabledLocations.length = 0;

	h.renderer._geometries.setGeometry(geometry, { count: h.pass });
	h.renderer._updateMorphtargets(mesh, geometry, program);
	h.renderer._vertexArrayBindings.setup(mesh, geometry, program);

	return program.uniforms.morphTargetInfluences;
}

// ---------------------------------------------------------------------------
// Geometry builders: 3 vertices, per-target deltas in different directions.
// ---------------------------------------------------------------------------

function basePosition(v) {
	return [v * 0.5 + 0.25, -v * 0.3, v * 0.125];
}

function baseNormal(v) {
	return [0.1 * v, 1 - 0.2 * v, -0.05 * v];
}

function positionDelta(i, v) {
	return [(i + 1) * 0.1 + v * 0.03, -((i + 1) * 0.07) + v * 0.02, (v - i) * 0.05];
}

function normalDelta(i, v) {
	const sign = v % 2 === 0 ? 1 : -1;
	return [sign * 0.02 * (i + 1), 0.01 * (v + 1) * (i % 2 === 0 ? 1 : -1), -0.03 * (i + 1)];
}

function makeAttribute(vectors, interleaved) {
	if (!interleaved) {
		const array = new Float32Array(vectors.length * 3);
		for (let i = 0; i < vectors.length; i++) {
			array[i * 3] = vectors[i][0];
			array[i * 3 + 1] = vectors[i][1];
			array[i * 3 + 2] = vectors[i][2];
		}
		return new Attribute(new Buffer(array, 3), 3);
	}

	// stride 5, offset 2; other components filled with garbage that must
	// never enter the deformation
	const stride = 5;
	const offset = 2;
	const array = new Float32Array(vectors.length * stride).fill(999);
	for (let i = 0; i < vectors.length; i++) {
		array[i * stride + offset] = vectors[i][0];
		array[i * stride + offset + 1] = vectors[i][1];
		array[i * stride + offset + 2] = vectors[i][2];
	}
	return new Attribute(new Buffer(array, stride), 3, offset);
}

function makeGeometry(vertexCount, targetCount, options) {
	options = options || {};

	const geometry = new Geometry();

	const positions = [];
	const normals = [];
	for (let v = 0; v < vertexCount; v++) {
		positions.push(basePosition(v));
		normals.push(baseNormal(v));
	}

	geometry.addAttribute('a_Position', makeAttribute(positions, options.interleaved));
	if (options.normals) {
		geometry.addAttribute('a_Normal', makeAttribute(normals, false));
	}

	const morphPositions = [];
	const morphNormals = [];
	for (let i = 0; i < targetCount; i++) {
		const deltas = [];
		const normalDeltas = [];
		for (let v = 0; v < vertexCount; v++) {
			deltas.push(positionDelta(i, v));
			normalDeltas.push(normalDelta(i, v));
		}
		morphPositions.push(makeAttribute(deltas, options.interleaved));
		if (options.normals) {
			morphNormals.push(makeAttribute(normalDeltas, options.interleaved));
		}
	}

	geometry.morphAttributes.position = morphPositions;
	if (options.normals) {
		geometry.morphAttributes.normal = morphNormals;
	}

	return geometry;
}

// Mixed signs and magnitudes; the first activeCount entries are non-zero.
function makeWeights(targetCount, activeCount) {
	const weights = new Array(targetCount).fill(0);
	for (let i = 0; i < activeCount; i++) {
		weights[i] = (i % 2 === 0 ? 0.4 : -0.3) * (1 + i * 0.1);
	}
	return weights;
}

// ---------------------------------------------------------------------------
// Independent summation helpers.
// ---------------------------------------------------------------------------

function readAttribute(attribute, v) {
	const array = attribute.buffer.array;
	const index = v * attribute.buffer.stride + attribute.offset;
	return [array[index], array[index + 1], array[index + 2]];
}

// Analytic expectation: base + sum of weight * delta over the source data.
function expectPosition(geometry, weights, v) {
	const p = readAttribute(geometry.getAttribute('a_Position'), v);
	const morphs = geometry.morphAttributes.position;
	for (let i = 0; i < morphs.length; i++) {
		const w = weights[i] || 0;
		if (w === 0) continue;
		const d = readAttribute(morphs[i], v);
		p[0] += d[0] * w;
		p[1] += d[1] * w;
		p[2] += d[2] * w;
	}
	return p;
}

function expectNormal(geometry, weights, v) {
	const n = readAttribute(geometry.getAttribute('a_Normal'), v);
	const morphs = geometry.morphAttributes.normal;
	for (let i = 0; i < morphs.length; i++) {
		const w = weights[i] || 0;
		if (w === 0) continue;
		const d = readAttribute(morphs[i], v);
		n[0] += d[0] * w;
		n[1] += d[1] * w;
		n[2] += d[2] * w;
	}
	return n;
}

// Rebuild the position the shader would compute from the currently bound
// slot attributes and the submitted morphTargetInfluences.
function reconstructPosition(geometry, uniforms, v) {
	const p = readAttribute(geometry.getAttribute('a_Position'), v);
	for (let k = 0; k < 8; k++) {
		const attribute = geometry.getAttribute('morphTarget' + k);
		const w = uniforms[k] || 0;
		if (!attribute || w === 0) continue;
		const d = readAttribute(attribute, v);
		p[0] += d[0] * w;
		p[1] += d[1] * w;
		p[2] += d[2] * w;
	}
	return p;
}

function reconstructNormal(geometry, uniforms, v) {
	const n = readAttribute(geometry.getAttribute('a_Normal'), v);
	for (let k = 0; k < 4; k++) {
		const attribute = geometry.getAttribute('morphNormal' + k);
		const w = uniforms[k] || 0;
		if (!attribute || w === 0) continue;
		const d = readAttribute(attribute, v);
		n[0] += d[0] * w;
		n[1] += d[1] * w;
		n[2] += d[2] * w;
	}
	return n;
}

function assertVec3Close(actual, expected, message) {
	for (let i = 0; i < 3; i++) {
		assert.ok(
			Math.abs(actual[i] - expected[i]) <= EPS,
			message + ' (component ' + i + ': ' + actual[i] + ' vs ' + expected[i] + ')'
		);
	}
}

function assertClose(actual, expected, message) {
	assert.ok(Math.abs(actual - expected) <= EPS, message + ' (' + actual + ' vs ' + expected + ')');
}

function checkConsistent(mesh, uniforms, vertexCount, hasNormals, label) {
	const geometry = mesh.geometry;
	const weights = mesh.morphTargetInfluences;

	for (let v = 0; v < vertexCount; v++) {
		const expected = expectPosition(geometry, weights, v);

		const cpu = mesh.getVertexPosition(v, new Vector3());
		assertVec3Close([cpu.x, cpu.y, cpu.z], expected, 'CPU position mismatch [' + label + '] vertex ' + v);

		const shader = reconstructPosition(geometry, uniforms, v);
		assertVec3Close(shader, expected, 'shader slot position mismatch [' + label + '] vertex ' + v);

		if (hasNormals) {
			const expectedNormal = expectNormal(geometry, weights, v);
			const shaderNormal = reconstructNormal(geometry, uniforms, v);
			assertVec3Close(shaderNormal, expectedNormal, 'shader slot normal mismatch [' + label + '] vertex ' + v);
		}
	}
}

function boundSlots(geometry, prefix) {
	let count = 0;
	while (geometry.getAttribute(prefix + count)) count++;
	return count;
}

function glBufferOf(h, buffer) {
	return h.renderer._buffers.get(buffer).glBuffer;
}

function assertAllBuffersReleased(h, label) {
	for (let i = 0; i < h.rec.createdBuffers.length; i++) {
		const buffer = h.rec.createdBuffers[i];
		assert.ok(h.rec.deletedBuffers.indexOf(buffer) !== -1, 'GL buffer ' + buffer.id + ' leaked [' + label + ']');
	}
}

// ---------------------------------------------------------------------------
// 1. Slot sweep: 0, 1, 4, 5, 8, 9 active targets (no morph normals).
// ---------------------------------------------------------------------------

{
	const h = createHarness();
	const program = createProgram(false);
	const vertexCount = 3;
	const targetCount = 9;
	const geometry = makeGeometry(vertexCount, targetCount);
	const mesh = new Mesh(geometry, null);

	const cases = [0, 1, 4, 5, 8, 9];

	for (let c = 0; c < cases.length; c++) {
		const activeCount = cases[c];
		mesh.morphTargetInfluences = makeWeights(targetCount, activeCount);

		const uniforms = draw(h, mesh, program);
		const label = 'active=' + activeCount;

		assert.equal(uniforms.length, 8, 'uniform length must be 8 without morph normals [' + label + ']');
		assert.equal(boundSlots(geometry, 'morphTarget'), Math.min(activeCount, 8), 'bound slot count [' + label + ']');

		if (activeCount <= 8) {
			for (let k = 0; k < activeCount; k++) {
				assert.equal(
					geometry.getAttribute('morphTarget' + k),
					geometry.morphAttributes.position[k],
					'fast path must bind the original target attribute [' + label + ']'
				);
				assertClose(uniforms[k], mesh.morphTargetInfluences[k], 'weight must be submitted as-is [' + label + ']');
			}
			for (let k = activeCount; k < 8; k++) {
				assert.equal(uniforms[k], 0, 'unused slot influence must be 0 [' + label + ']');
			}
		} else {
			for (let k = 0; k < 8; k++) {
				assert.equal(uniforms[k], 1, 'overflow slot influence must be 1 [' + label + ']');
				assert.ok(
					geometry.morphAttributes.position.indexOf(geometry.getAttribute('morphTarget' + k)) === -1,
					'overflow slots must be composite attributes [' + label + ']'
				);

				// slot k holds the independent sum of active targets k, k + 8, ...
				for (let v = 0; v < vertexCount; v++) {
					const expected = [0, 0, 0];
					for (let j = k; j < activeCount; j += 8) {
						const w = mesh.morphTargetInfluences[j];
						const d = readAttribute(geometry.morphAttributes.position[j], v);
						expected[0] += d[0] * w;
						expected[1] += d[1] * w;
						expected[2] += d[2] * w;
					}
					assertVec3Close(readAttribute(geometry.getAttribute('morphTarget' + k), v), expected, 'composite slot mismatch [' + label + ']');

					const glData = glBufferOf(h, geometry.getAttribute('morphTarget' + k).buffer).data;
					assertVec3Close([glData[v * 3], glData[v * 3 + 1], glData[v * 3 + 2]], expected, 'uploaded composite mismatch [' + label + ']');
				}
			}
		}

		checkConsistent(mesh, uniforms, vertexCount, false, label);
	}

	geometry.dispose();
	assertAllBuffersReleased(h, 'slot sweep');
}

// ---------------------------------------------------------------------------
// 2. Negative weights.
// ---------------------------------------------------------------------------

{
	const h = createHarness();
	const program = createProgram(false);
	const geometry = makeGeometry(3, 9);
	const mesh = new Mesh(geometry, null);

	mesh.morphTargetInfluences = [-0.5, 0, 0, 0, 0, 0, 0, 0, 0];

	const uniforms = draw(h, mesh, program);

	assert.equal(boundSlots(geometry, 'morphTarget'), 1, 'negative weight target must be active');
	assertClose(uniforms[0], -0.5, 'negative weight must reach the uniform');
	assert.equal(geometry.getAttribute('morphTarget0'), geometry.morphAttributes.position[0]);

	checkConsistent(mesh, uniforms, 3, false, 'negative weight');

	// negative weights inside an overflow composite
	mesh.morphTargetInfluences = makeWeights(9, 9);
	mesh.morphTargetInfluences[0] = -0.5;
	mesh.morphTargetInfluences[4] = -1.25;

	const uniforms2 = draw(h, mesh, program);
	checkConsistent(mesh, uniforms2, 3, false, 'negative weights in overflow');

	geometry.dispose();
	assertAllBuffersReleased(h, 'negative weights');
}

// ---------------------------------------------------------------------------
// 3. Morph normals: 4 position/normal pairs.
// ---------------------------------------------------------------------------

{
	const h = createHarness();
	const program = createProgram(true);
	const geometry = makeGeometry(3, 5, { normals: true });
	const mesh = new Mesh(geometry, null);

	// 3 active targets fit the 4 pairs: direct binding
	mesh.morphTargetInfluences = makeWeights(5, 3);

	let uniforms = draw(h, mesh, program);

	assert.equal(uniforms.length, 4, 'uniform length must be 4 with morph normals');
	assert.equal(boundSlots(geometry, 'morphTarget'), 3);
	assert.equal(boundSlots(geometry, 'morphNormal'), 3);
	assert.equal(geometry.getAttribute('morphNormal1'), geometry.morphAttributes.normal[1], 'fast path must bind the original normal attribute');
	assertClose(uniforms[2], mesh.morphTargetInfluences[2], 'weight must be submitted as-is');

	checkConsistent(mesh, uniforms, 3, true, 'normals direct');

	// 5 active targets overflow the 4 pairs: composite positions and normals
	mesh.morphTargetInfluences = makeWeights(5, 5);

	uniforms = draw(h, mesh, program);

	assert.equal(uniforms.length, 4);
	assert.deepEqual(uniforms, [1, 1, 1, 1], 'overflow slot influences must be 1');
	assert.equal(boundSlots(geometry, 'morphTarget'), 4);
	assert.equal(boundSlots(geometry, 'morphNormal'), 4);

	// slot 0 holds targets 0 and 4 (k, k + 4)
	for (let v = 0; v < 3; v++) {
		const expected = [0, 0, 0];
		for (let j = 0; j < 5; j += 4) {
			const w = mesh.morphTargetInfluences[j];
			const d = readAttribute(geometry.morphAttributes.normal[j], v);
			expected[0] += d[0] * w;
			expected[1] += d[1] * w;
			expected[2] += d[2] * w;
		}
		assertVec3Close(readAttribute(geometry.getAttribute('morphNormal0'), v), expected, 'composite normal slot mismatch');
	}

	checkConsistent(mesh, uniforms, 3, true, 'normals overflow');

	geometry.dispose();
	assertAllBuffersReleased(h, 'morph normals');
}

// ---------------------------------------------------------------------------
// 4. Interleaved buffers with offsets.
// ---------------------------------------------------------------------------

{
	const h = createHarness();
	const program = createProgram(false);
	const geometry = makeGeometry(3, 9, { interleaved: true });
	const mesh = new Mesh(geometry, null);

	// fast path: interleaved sources are bound directly with stride/offset
	mesh.morphTargetInfluences = makeWeights(9, 2);

	let uniforms = draw(h, mesh, program);

	checkConsistent(mesh, uniforms, 3, false, 'interleaved direct');

	const programAttributes = program.getAttributes();
	const morphCall = h.rec.pointerCalls.find(call => call.location === programAttributes.morphTarget0.location);
	assert.ok(morphCall, 'morphTarget0 must be bound');
	assert.equal(morphCall.buffer, glBufferOf(h, geometry.morphAttributes.position[0].buffer), 'fast path must point at the source buffer');
	assert.equal(morphCall.stride, 5 * 4, 'byte stride must reach vertexAttribPointer');
	assert.equal(morphCall.offset, 2 * 4, 'byte offset must reach vertexAttribPointer');

	// overflow: composites must contain only the addressed components
	mesh.morphTargetInfluences = makeWeights(9, 9);

	uniforms = draw(h, mesh, program);

	checkConsistent(mesh, uniforms, 3, false, 'interleaved overflow');

	for (let k = 0; k < 8; k++) {
		const data = geometry.getAttribute('morphTarget' + k).buffer.array;
		for (let i = 0; i < data.length; i++) {
			assert.ok(Math.abs(data[i]) < 100, 'interleaved garbage components must not enter the composite');
		}
	}

	geometry.dispose();
	assertAllBuffersReleased(h, 'interleaved');
}

// ---------------------------------------------------------------------------
// 5. Refresh of reused data: source buffer updates, weight changes,
//    target attribute replacement, added targets.
// ---------------------------------------------------------------------------

{
	const h = createHarness();
	const program = createProgram(false);
	const geometry = makeGeometry(3, 9);
	const mesh = new Mesh(geometry, null);

	// fast path: in-place source buffer update is picked up after version++
	mesh.morphTargetInfluences = makeWeights(9, 2);
	draw(h, mesh, program);

	const source = geometry.morphAttributes.position[0];
	const sourceGL = glBufferOf(h, source.buffer);
	const sourceUploads = sourceGL.uploadCount;

	source.buffer.array[0] = 5;
	source.buffer.array[1] = -2;
	source.buffer.array[2] = 1;
	source.buffer.version++;

	let uniforms = draw(h, mesh, program);

	assert.ok(sourceGL.uploadCount > sourceUploads, 'updated source buffer must be re-uploaded');
	checkConsistent(mesh, uniforms, 3, false, 'source update direct');

	// overflow: source buffer update must trigger a recomposite
	mesh.morphTargetInfluences = makeWeights(9, 9);
	uniforms = draw(h, mesh, program);
	checkConsistent(mesh, uniforms, 3, false, 'overflow before update');

	const tempAttribute = geometry.getAttribute('morphTarget0');
	const tempGL = glBufferOf(h, tempAttribute.buffer);
	const tempUploads = tempGL.uploadCount;

	source.buffer.array[0] = 7;
	source.buffer.version++;

	uniforms = draw(h, mesh, program);

	assert.ok(tempGL.uploadCount > tempUploads, 'composite must be re-uploaded after a source buffer update');
	checkConsistent(mesh, uniforms, 3, false, 'source update overflow');

	// weight change only: recomposite into the reused buffers, no new GL buffers
	const createdBefore = h.rec.createdBuffers.length;
	mesh.morphTargetInfluences = makeWeights(9, 9).map(w => w * 1.5);

	uniforms = draw(h, mesh, program);

	assert.equal(geometry.getAttribute('morphTarget0'), tempAttribute, 'composite buffers must be reused');
	assert.equal(h.rec.createdBuffers.length, createdBefore, 'recomposite must not allocate new GL buffers');
	checkConsistent(mesh, uniforms, 3, false, 'weight change overflow');

	// replacing a target attribute must be picked up
	const replacedAttribute = geometry.morphAttributes.position[3];
	const replacementDeltas = [];
	for (let v = 0; v < 3; v++) replacementDeltas.push([0.3 + v, -0.2 * v, 0.1]);
	geometry.morphAttributes.position[3] = makeAttribute(replacementDeltas, false);

	uniforms = draw(h, mesh, program);
	checkConsistent(mesh, uniforms, 3, false, 'target attribute replaced');

	// the engine only releases buffers still referenced by the geometry, so
	// the replaced buffer is released explicitly through WebGLBuffers
	h.renderer._buffers.removeBuffer(replacedAttribute.buffer);

	// adding a target must be picked up
	const extraDeltas = [];
	for (let v = 0; v < 3; v++) extraDeltas.push(positionDelta(9, v));
	geometry.morphAttributes.position.push(makeAttribute(extraDeltas, false));
	mesh.morphTargetInfluences = makeWeights(9, 9).concat([0.35]);

	uniforms = draw(h, mesh, program);
	checkConsistent(mesh, uniforms, 3, false, 'target added');

	geometry.dispose();
	assertAllBuffersReleased(h, 'refresh');
}

// ---------------------------------------------------------------------------
// 6. Shared geometry, A / B / A order with different weights.
// ---------------------------------------------------------------------------

{
	const h = createHarness();
	const program = createProgram(false);
	const geometry = makeGeometry(3, 9);
	const meshA = new Mesh(geometry, null);
	const meshB = new Mesh(geometry, null);

	meshA.morphTargetInfluences = makeWeights(9, 9); // overflow
	meshB.morphTargetInfluences = makeWeights(9, 3); // direct

	const uniformsA1 = draw(h, meshA, program);
	assert.deepEqual(uniformsA1, [1, 1, 1, 1, 1, 1, 1, 1]);
	checkConsistent(meshA, uniformsA1, 3, false, 'A first');

	const tempAttribute = geometry.getAttribute('morphTarget0');
	const tempGL = glBufferOf(h, tempAttribute.buffer);
	const tempUploads = tempGL.uploadCount;

	// same pass: setGeometry is skipped, bindings must still follow the object
	const uniformsB = draw(h, meshB, program, false);
	assert.equal(geometry.getAttribute('morphTarget0'), geometry.morphAttributes.position[0], 'B must bind its own targets');
	assertClose(uniformsB[0], meshB.morphTargetInfluences[0], 'B weights must be submitted');
	checkConsistent(meshB, uniformsB, 3, false, 'B');

	const uniformsA2 = draw(h, meshA, program, false);
	assert.equal(geometry.getAttribute('morphTarget0'), tempAttribute, 'A must reuse its composite buffers');
	assert.equal(tempGL.uploadCount, tempUploads, 'unchanged composite must not be re-uploaded');
	checkConsistent(meshA, uniformsA2, 3, false, 'A again');

	// changed weights for A must recomposite even when setGeometry is skipped
	meshA.morphTargetInfluences = makeWeights(9, 9).map(w => w * 1.1);
	const uniformsA3 = draw(h, meshA, program, false);
	assert.ok(tempGL.uploadCount > tempUploads, 'recomposite must upload even when the pass skips setGeometry');
	checkConsistent(meshA, uniformsA3, 3, false, 'A changed');

	geometry.dispose();
	assertAllBuffersReleased(h, 'shared geometry A/B/A');
}

// ---------------------------------------------------------------------------
// 7. Transitions on one geometry: normals -> no normals -> overflow ->
//    back to few active -> all weights zero. No stale slots or uniforms.
// ---------------------------------------------------------------------------

{
	const h = createHarness();
	const normalsProgram = createProgram(true);
	const plainProgram = createProgram(false);
	const geometry = makeGeometry(3, 9, { normals: true });
	const mesh = new Mesh(geometry, null);

	// overflow with morph normals: 5 active targets, 4 pairs
	mesh.morphTargetInfluences = makeWeights(9, 5);
	let uniforms = draw(h, mesh, normalsProgram);
	assert.equal(uniforms.length, 4);
	assert.equal(boundSlots(geometry, 'morphNormal'), 4);
	checkConsistent(mesh, uniforms, 3, true, 'normals overflow');

	const tempPosGL = glBufferOf(h, geometry.getAttribute('morphTarget0').buffer);
	const tempNorGL = glBufferOf(h, geometry.getAttribute('morphNormal0').buffer);

	// geometry loses its morph normals: 8 slots, 5 active targets fit directly
	const removedNormals = geometry.morphAttributes.normal;
	delete geometry.morphAttributes.normal;
	uniforms = draw(h, mesh, plainProgram);
	assert.equal(uniforms.length, 8, 'uniform length must follow the slot layout');
	assert.equal(boundSlots(geometry, 'morphNormal'), 0, 'normal slots must not remain');
	assert.equal(boundSlots(geometry, 'morphTarget'), 5);
	assert.equal(geometry.getAttribute('morphTarget4'), geometry.morphAttributes.position[4]);
	checkConsistent(mesh, uniforms, 3, false, 'normals removed');

	// the engine only releases buffers still referenced by the geometry, so
	// the removed normal buffers are released explicitly through WebGLBuffers
	for (let i = 0; i < removedNormals.length; i++) {
		h.renderer._buffers.removeBuffer(removedNormals[i].buffer);
	}

	// 9 active targets overflow the 8 slots: new composites replace the pairs
	mesh.morphTargetInfluences = makeWeights(9, 9);
	uniforms = draw(h, mesh, plainProgram);
	assert.deepEqual(uniforms, [1, 1, 1, 1, 1, 1, 1, 1]);
	assert.equal(boundSlots(geometry, 'morphTarget'), 8);
	assert.ok(h.rec.deletedBuffers.indexOf(tempPosGL) !== -1, 'obsolete composite buffers must be released');
	assert.ok(h.rec.deletedBuffers.indexOf(tempNorGL) !== -1, 'obsolete composite normal buffers must be released');
	checkConsistent(mesh, uniforms, 3, false, 'overflow after normals removed');

	// back to a few active targets: no stale slots may remain
	mesh.morphTargetInfluences = makeWeights(9, 2);
	uniforms = draw(h, mesh, plainProgram);
	assert.equal(boundSlots(geometry, 'morphTarget'), 2, 'old slots must not remain');
	assert.equal(geometry.getAttribute('morphTarget2'), undefined);
	assert.equal(uniforms[2], 0);
	assert.equal(uniforms[7], 0);
	checkConsistent(mesh, uniforms, 3, false, 'back to few active');

	// all weights zero: nothing bound, all-zero uniform
	mesh.morphTargetInfluences = new Array(9).fill(0);
	uniforms = draw(h, mesh, plainProgram);
	assert.equal(boundSlots(geometry, 'morphTarget'), 0, 'no slot may remain when all weights are zero');
	assert.deepEqual(uniforms, [0, 0, 0, 0, 0, 0, 0, 0]);
	checkConsistent(mesh, uniforms, 3, false, 'all weights zero');

	geometry.dispose();
	assertAllBuffersReleased(h, 'transitions');
}

// ---------------------------------------------------------------------------
// 8. Caller data must not be mutated.
// ---------------------------------------------------------------------------

{
	const h = createHarness();
	const program = createProgram(false);
	const geometry = makeGeometry(3, 9);
	const mesh = new Mesh(geometry, null);

	mesh.morphTargetInfluences = makeWeights(9, 9);

	const weightsSnapshot = mesh.morphTargetInfluences.slice();
	const positionSnapshot = Array.from(geometry.getAttribute('a_Position').buffer.array);
	const sourceSnapshot = geometry.morphAttributes.position.map(attribute => Array.from(attribute.buffer.array));

	draw(h, mesh, program);
	draw(h, mesh, program);

	assert.deepEqual(mesh.morphTargetInfluences, weightsSnapshot, 'caller weight array must not be modified');
	assert.deepEqual(Array.from(geometry.getAttribute('a_Position').buffer.array), positionSnapshot, 'a_Position must not be modified');
	for (let i = 0; i < sourceSnapshot.length; i++) {
		assert.deepEqual(
			Array.from(geometry.morphAttributes.position[i].buffer.array),
			sourceSnapshot[i],
			'source morph target ' + i + ' must not be modified'
		);
	}

	geometry.dispose();
	assertAllBuffersReleased(h, 'no mutation');
}

console.log('MORPH_SUBMIT_OK');
