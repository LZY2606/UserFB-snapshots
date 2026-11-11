// Regression tests for the CPU 2x2 fragment-quad executor
// (debugFragmentQuad in wgsl_reflect.module.js).
//
// Run with: node test/quad_derivatives.test.mjs
//
// The expected values below are derived independently from the derivative
// definition, NOT from the executor's own output:
//
//   quad lane order           0=TL 1=TR 2=BL 3=BR
//   dx(Fine)  = right-left within the lane's row
//   dy(Fine)  = bottom-top within the lane's column
//   dx(Coarse)= TR-TL for every lane
//   dy(Coarse)= BL-TL for every lane
//   fwidth    = |dx| + |dy| (same variant for both terms)
//   unqualified dpdx/dpdy/fwidth are Coarse.
//
// This is expressed as a table of linear weights over the four lane inputs,
// which is the mathematical definition of the operation. Hard-coded expected
// arrays for the asymmetric base quad [1,4,8,20] provide a second, independent
// check on the arithmetic.

import assert from "node:assert/strict";
import { debugFragmentQuad } from "../wgsl_reflect.module.js";

const TOLERANCE = 1e-5;
const LANE_NAMES = ["TL", "TR", "BL", "BR"]; // 左上, 右上, 左下, 右下
const COMPONENT_NAMES = ["x", "y", "z", "w"];

// -----------------------------------------------------------------------------
// Independent reference model
// -----------------------------------------------------------------------------
// For each lane, the derivative is a linear combination of the four lane
// input values: result[lane] = sum_k weight[lane][k] * input[k].
// fwidth is the componentwise |dx| + |dy| of the selected variant.

const FINE_DX = [
  [-1,  1,  0,  0], // lane 0 TL: TR - TL
  [-1,  1,  0,  0], // lane 1 TR: TR - TL
  [ 0,  0, -1,  1], // lane 2 BL: BR - BL
  [ 0,  0, -1,  1], // lane 3 BR: BR - BL
];
const FINE_DY = [
  [-1,  0,  1,  0], // lane 0 TL: BL - TL
  [ 0, -1,  0,  1], // lane 1 TR: BR - TR
  [-1,  0,  1,  0], // lane 2 BL: BL - TL
  [ 0, -1,  0,  1], // lane 3 BR: BR - TR
];
const COARSE_DX = [
  [-1,  1,  0,  0], // every lane: TR - TL
  [-1,  1,  0,  0],
  [-1,  1,  0,  0],
  [-1,  1,  0,  0],
];
const COARSE_DY = [
  [-1,  0,  1,  0], // every lane: BL - TL
  [-1,  0,  1,  0],
  [-1,  0,  1,  0],
  [-1,  0,  1,  0],
];

function derivativeVariant(name) {
  if (name.endsWith("Fine")) return "Fine";
  return "Coarse"; // bare names and *Coarse are all Coarse
}

function derivativeOp(name) {
  if (name.startsWith("dpdx")) return "dpdx";
  if (name.startsWith("dpdy")) return "dpdy";
  if (name.startsWith("fwidth")) return "fwidth";
  throw new Error(`unknown derivative ${name}`);
}

function weightsFor(op, variant) {
  if (op === "dpdx") return variant === "Fine" ? FINE_DX : COARSE_DX;
  if (op === "dpdy") return variant === "Fine" ? FINE_DY : COARSE_DY;
  throw new Error(`no linear weights for ${op}`);
}

function linearCombination(lane, weight, laneValues, component) {
  let value = 0;
  for (let k = 0; k < 4; ++k) {
    value += weight[lane][k] * laneValues[k][component];
  }
  return value;
}

// laneValues: [ [c0,c1,...] for TL, TR, BL, BR ]
// returns:     [ [c0,c1,...] for TL, TR, BL, BR ]
function referenceDerivative(name, laneValues) {
  const variant = derivativeVariant(name);
  const op = derivativeOp(name);
  const componentCount = laneValues[0].length;
  const perLane = [];
  for (let lane = 0; lane < 4; ++lane) {
    const components = [];
    for (let comp = 0; comp < componentCount; ++comp) {
      if (op === "fwidth") {
        const dx = linearCombination(lane, weightsFor("dpdx", variant), laneValues, comp);
        const dy = linearCombination(lane, weightsFor("dpdy", variant), laneValues, comp);
        components.push(Math.abs(dx) + Math.abs(dy));
      } else {
        components.push(linearCombination(lane, weightsFor(op, variant), laneValues, comp));
      }
    }
    perLane.push(components);
  }
  return perLane;
}

// Hard-coded expectations for the asymmetric scalar quad [1,4,8,20].
const BASE_SCALAR_QUAD = [1, 4, 8, 20];
const BASE_SCALAR_EXPECTED = {
  dpdx:        [3, 3, 3, 3],       // Coarse dx
  dpdxCoarse:  [3, 3, 3, 3],
  dpdxFine:    [3, 3, 12, 12],     // row-local dx
  dpdy:        [7, 7, 7, 7],       // Coarse dy
  dpdyCoarse:  [7, 7, 7, 7],
  dpdyFine:    [7, 16, 7, 16],     // column-local dy
  fwidth:      [10, 10, 10, 10],   // |3|+|7|
  fwidthCoarse:[10, 10, 10, 10],
  fwidthFine:  [10, 19, 19, 28],   // |dxFine|+|dyFine|
};

// Swap rows: [BL, BR, TL, TR]   Swap columns: [TR, TL, BR, BL]
const SWAPPED_ROWS_EXPECTED = {
  dpdx:        [12, 12, 12, 12],
  dpdxCoarse:  [12, 12, 12, 12],
  dpdxFine:    [12, 12, 3, 3],
  dpdy:        [-7, -7, -7, -7],
  dpdyCoarse:  [-7, -7, -7, -7],
  dpdyFine:    [-7, -16, -7, -16],
  fwidth:      [19, 19, 19, 19],
  fwidthCoarse:[19, 19, 19, 19],
  fwidthFine:  [19, 28, 10, 19],
};
const SWAPPED_COLUMNS_EXPECTED = {
  dpdx:        [-3, -3, -3, -3],
  dpdxCoarse:  [-3, -3, -3, -3],
  dpdxFine:    [-3, -3, -12, -12],
  dpdy:        [16, 16, 16, 16],
  dpdyCoarse:  [16, 16, 16, 16],
  dpdyFine:    [16, 7, 16, 7],
  fwidth:      [19, 19, 19, 19],
  fwidthCoarse:[19, 19, 19, 19],
  fwidthFine:  [19, 10, 28, 19],
};

const DERIVATIVE_NAMES = [
  "dpdx", "dpdxFine", "dpdxCoarse",
  "dpdy", "dpdyFine", "dpdyCoarse",
  "fwidth", "fwidthFine", "fwidthCoarse",
];

// -----------------------------------------------------------------------------
// Shader generators. Derivative arguments use only stage inputs and local
// arithmetic (never a custom function call), matching the executor's stated
// capabilities.
// -----------------------------------------------------------------------------

function derivativeShader(typeName, derivativeName) {
  let buildResult;
  if (typeName === "f32") {
    buildResult = "vec4f(d, 0.0, 0.0, 0.0)";
  } else if (typeName === "vec2f") {
    buildResult = "vec4f(d.x, d.y, 0.0, 0.0)";
  } else if (typeName === "vec3f") {
    buildResult = "vec4f(d.x, d.y, d.z, 0.0)";
  } else if (typeName === "vec4f") {
    buildResult = "d";
  } else {
    throw new Error(`unsupported test type ${typeName}`);
  }
  return `
@fragment
fn main(@location(0) x: ${typeName}) -> @location(0) vec4f {
  let d = ${derivativeName}(x);
  return ${buildResult};
}`;
}

function discardShader(derivativeName, discardAfterDerivative) {
  const derivative = `  let d = ${derivativeName}(x);`;
  const discard = `  if (dflag > 0.5) { discard; }`;
  return `
@fragment
fn main(@location(0) x: f32, @location(1) dflag: f32) -> @location(0) vec4f {
${discardAfterDerivative ? derivative + "\n" + discard : discard + "\n" + derivative}
  return vec4f(d, 0.0, 0.0, 0.0);
}`;
}

// Same AST derivative call site in four loop iterations, with a different
// argument value each iteration. The four components hold iteration 0..3.
function loopShader(derivativeName) {
  return `
@fragment
fn main(@location(0) x: f32) -> @location(0) vec4f {
  var out = vec4f(0.0);
  for (var i = 0; i < 4; i = i + 1) {
    let s = f32(i + 1);
    let d = ${derivativeName}(x * s);
    if (i == 0) { out.x = d; }
    else if (i == 1) { out.y = d; }
    else if (i == 2) { out.z = d; }
    else { out.w = d; }
  }
  return out;
}`;
}

// Shader used for the interleaving / state-isolation test. It discards in a
// lane-specific way and computes both Fine and Coarse derivatives.
const INTERLEAVE_SHADER_A = `
@fragment
fn main(@location(0) x: f32, @location(1) dflag: f32) -> @location(0) vec4f {
  if (dflag > 0.5) { discard; }
  let dxf = dpdxFine(x);
  let dyf = dpdyFine(x);
  let fw = fwidthFine(x);
  let dxc = dpdxCoarse(x);
  return vec4f(dxf, dyf, fw, dxc);
}`;

const INTERLEAVE_SHADER_B = `
@fragment
fn main(@location(0) v: vec2f) -> @location(0) vec4f {
  let dx = dpdxFine(v);
  let dy = dpdyFine(v);
  return vec4f(dx.x, dx.y, dy.x, dy.y);
}`;

// -----------------------------------------------------------------------------
// Checking helpers. Every failure names the case, derivative, lane and
// component.
// -----------------------------------------------------------------------------

let comparisonCount = 0;
const completedCases = [];

function laneLabel(lane) {
  return `lane ${lane} (${LANE_NAMES[lane]})`;
}

function componentLabel(comp, componentCount) {
  if (componentCount === 1) return "component 0 (value)";
  const name = COMPONENT_NAMES[comp] ?? `[${comp}]`;
  return `component ${comp} (${name})`;
}

function assertClose(actual, expected, message) {
  ++comparisonCount;
  assert.ok(
    typeof actual === "number" && Number.isFinite(actual),
    `${message}: expected a finite number, got ${String(actual)}`,
  );
  assert.ok(
    Math.abs(actual - expected) <= TOLERANCE,
    `${message}: expected ${expected}, got ${actual} (|diff|=${Math.abs(actual - expected)} > ${TOLERANCE})`,
  );
}

// Compare one lane's output components against expected derivative components.
// `actual` is expected to be the padded vec4 output array.
function assertLaneValues(caseName, derivativeName, lane, actual, expectedComponents) {
  assert.ok(
    Array.isArray(actual),
    `${caseName} | ${derivativeName} | ${laneLabel(lane)} | expected an output array, got ${JSON.stringify(actual)}`,
  );
  assert.equal(
    actual.length, 4,
    `${caseName} | ${derivativeName} | ${laneLabel(lane)} | output width`,
  );
  for (let comp = 0; comp < 4; ++comp) {
    const expected = comp < expectedComponents.length ? expectedComponents[comp] : 0;
    assertClose(
      actual[comp],
      expected,
      `${caseName} | ${derivativeName} | ${laneLabel(lane)} | ${componentLabel(comp, expectedComponents.length)}`,
    );
  }
}

// Run a uniform-control-flow case and check every lane/component.
function runUniformCase({ caseName, shader, quad, expectedPerLane, derivativeName }) {
  const result = debugFragmentQuad(shader, "main", quad, {});
  assert.equal(result.outputs.length, 4, `${caseName} | outputs length`);
  assert.equal(result.discarded.length, 4, `${caseName} | discarded length`);
  assert.ok(result.errors.length === 0,
    `${caseName} | unexpected errors: ${result.errors.join("; ")}`);

  for (let lane = 0; lane < 4; ++lane) {
    assert.equal(result.discarded[lane], false,
      `${caseName} | ${derivativeName} | ${laneLabel(lane)} | unexpectedly discarded`);
    assertLaneValues(caseName, derivativeName, lane, result.outputs[lane], expectedPerLane[lane]);
  }
  completedCases.push(caseName);
  return result;
}

function padScalar(values) {
  return values.map((v) => [v, 0, 0, 0]);
}

function laneVectorsFromScalars(values) {
  return values.map((v) => [v]);
}

function laneVectorsFromComponentQuad(componentQuad, componentCount) {
  // componentQuad[c] is the scalar quad [TL,TR,BL,BR] for component c.
  const lanes = [[], [], [], []];
  for (let lane = 0; lane < 4; ++lane) {
    for (let comp = 0; comp < componentCount; ++comp) {
      lanes[lane].push(componentQuad[comp][lane]);
    }
  }
  return lanes;
}

// -----------------------------------------------------------------------------
// Deterministic seeded data (fixed seed -> identical data on every run).
// Values are quarter-integers in [-20,20], exactly representable in f32, and
// include positive and negative components.
// -----------------------------------------------------------------------------

function mulberry32(seed) {
  let state = seed >>> 0;
  return function next() {
    state = (state + 0x6D2B79F5) >>> 0;
    let t = state;
    t = Math.imul(t ^ (t >>> 15), t | 1);
    t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}

const SEED = 0xC0FFEE;
const rng = mulberry32(SEED);

function randomQuarter() {
  // integer in [-80, 80] divided by 4 -> multiples of 0.25 in [-20, 20].
  return (Math.floor(rng() * 161) - 80) / 4;
}

function makeSeededVec4Quad() {
  const lanes = [];
  for (let lane = 0; lane < 4; ++lane) {
    lanes.push([randomQuarter(), randomQuarter(), randomQuarter(), randomQuarter()]);
  }
  return lanes;
}

const SEEDED_VEC4_QUAD = makeSeededVec4Quad();

// Add a constant vector to every lane (quad translation). Componentwise
// derivatives must not change.
const TRANSLATION = [3.25, -5.5, 7.75, -2.25];
const TRANSLATED_VEC4_QUAD = SEEDED_VEC4_QUAD.map(
  (lane) => lane.map((value, comp) => value + TRANSLATION[comp]),
);

function scaleQuad(quad, factor) {
  return quad.map((lane) => lane.map((value) => value * factor));
}

// -----------------------------------------------------------------------------
// Test cases
// -----------------------------------------------------------------------------

function testExplicitScalarBase() {
  const casePrefix = "explicit-base-scalar[1,4,8,20]";
  const quad = laneVectorsFromScalars(BASE_SCALAR_QUAD).map((lane) => ({ 0: lane[0] }));
  for (const name of DERIVATIVE_NAMES) {
    const expectedPerLane = padScalar(BASE_SCALAR_EXPECTED[name]);
    runUniformCase({
      caseName: `${casePrefix}/${name}`,
      shader: derivativeShader("f32", name),
      quad,
      expectedPerLane,
      derivativeName: name,
    });
  }
}

function testTypesAndComponents() {
  // Fixed asymmetric quad with distinct, signed values per component:
  //   comp0: [ 1,  4,   8, 20]
  //   comp1: [-3,  5,  -7, 11]
  //   comp2: [ 2, -6,  10,-14]
  //   comp3: [-1,  3,  -5,  8]
  const componentQuad = [
    [1, 4, 8, 20],
    [-3, 5, -7, 11],
    [2, -6, 10, -14],
    [-1, 3, -5, 8],
  ];
  const types = [
    { typeName: "f32", componentCount: 1 },
    { typeName: "vec2f", componentCount: 2 },
    { typeName: "vec3f", componentCount: 3 },
    { typeName: "vec4f", componentCount: 4 },
  ];
  const signedScalarQuad = [[-3], [7], [-9], [15]];

  for (const { typeName, componentCount } of types) {
    const laneValues = typeName === "f32"
      ? signedScalarQuad
      : laneVectorsFromComponentQuad(componentQuad, componentCount);
    const quad = laneValues.map((lane) => ({
      0: componentCount === 1 ? lane[0] : lane,
    }));
    for (const name of DERIVATIVE_NAMES) {
      const expectedPerLane = referenceDerivative(name, laneValues);
      runUniformCase({
        caseName: `types/${typeName}/${name}`,
        shader: derivativeShader(typeName, name),
        quad,
        expectedPerLane,
        derivativeName: name,
      });
    }
  }
}

function testSeededSignedData() {
  for (const name of DERIVATIVE_NAMES) {
    const expectedPerLane = referenceDerivative(name, SEEDED_VEC4_QUAD);
    runUniformCase({
      caseName: `seeded-signed/vec4f/${name}`,
      shader: derivativeShader("vec4f", name),
      quad: SEEDED_VEC4_QUAD.map((lane) => ({ 0: lane })),
      expectedPerLane,
      derivativeName: name,
    });
  }
}

function testConstantTranslation() {
  for (const name of DERIVATIVE_NAMES) {
    const expectedPerLane = referenceDerivative(name, SEEDED_VEC4_QUAD);
    runUniformCase({
      caseName: `constant-translation/vec4f/${name}`,
      shader: derivativeShader("vec4f", name),
      quad: TRANSLATED_VEC4_QUAD.map((lane) => ({ 0: lane })),
      expectedPerLane,
      derivativeName: name,
    });
  }
}

function testInputScaling() {
  for (const factor of [2.5, -3.0]) {
    const scaledQuad = scaleQuad(SEEDED_VEC4_QUAD, factor);
    // Independent reference on the scaled input. Also verify the expected
    // scaling relation against the original reference values.
    for (const name of DERIVATIVE_NAMES) {
      const baseExpected = referenceDerivative(name, SEEDED_VEC4_QUAD);
      const expectedPerLane = referenceDerivative(name, scaledQuad);
      const op = derivativeOp(name);
      const expectedFactor = op === "fwidth" ? Math.abs(factor) : factor;
      for (let lane = 0; lane < 4; ++lane) {
        for (let comp = 0; comp < 4; ++comp) {
          assertClose(
            expectedPerLane[lane][comp],
            baseExpected[lane][comp] * expectedFactor,
            `input-scaling/${factor}/${name} | reference relation | ${laneLabel(lane)} | component ${comp}`,
          );
        }
      }
      runUniformCase({
        caseName: `input-scaling/${factor}/vec4f/${name}`,
        shader: derivativeShader("vec4f", name),
        quad: scaledQuad.map((lane) => ({ 0: lane })),
        expectedPerLane,
        derivativeName: name,
      });
    }
  }
}

function testRowAndColumnSwap() {
  const rowsQuad = [8, 20, 1, 4].map((v) => ({ 0: v }));    // [BL, BR, TL, TR]
  const colsQuad = [4, 1, 20, 8].map((v) => ({ 0: v }));    // [TR, TL, BR, BL]
  for (const name of DERIVATIVE_NAMES) {
    runUniformCase({
      caseName: `swap-rows/scalar/${name}`,
      shader: derivativeShader("f32", name),
      quad: rowsQuad,
      expectedPerLane: padScalar(SWAPPED_ROWS_EXPECTED[name]),
      derivativeName: name,
    });
    runUniformCase({
      caseName: `swap-columns/scalar/${name}`,
      shader: derivativeShader("f32", name),
      quad: colsQuad,
      expectedPerLane: padScalar(SWAPPED_COLUMNS_EXPECTED[name]),
      derivativeName: name,
    });
  }
}

function testLoopWithChangingArguments() {
  const quad = laneVectorsFromScalars(BASE_SCALAR_QUAD).map((lane) => ({ 0: lane[0] }));
  for (const name of DERIVATIVE_NAMES) {
    const caseName = `loop-changing-args/scalar/${name}`;
    const result = debugFragmentQuad(loopShader(name), "main", quad, {});
    assert.equal(result.outputs.length, 4, `${caseName} | outputs length`);
    assert.ok(result.errors.length === 0,
      `${caseName} | unexpected errors: ${result.errors.join("; ")}`);
    for (let lane = 0; lane < 4; ++lane) {
      assert.equal(result.discarded[lane], false,
        `${caseName} | ${laneLabel(lane)} | unexpectedly discarded`);
      assert.ok(Array.isArray(result.outputs[lane]),
        `${caseName} | ${laneLabel(lane)} | expected an output array`);
      assert.equal(result.outputs[lane].length, 4,
        `${caseName} | ${laneLabel(lane)} | output width`);
      for (let iteration = 0; iteration < 4; ++iteration) {
        // Iteration i uses x * (i+1); the derivative is recomputed for that
        // same AST call site each time.
        const scaledLanes = laneVectorsFromScalars(
          BASE_SCALAR_QUAD.map((value) => value * (iteration + 1)),
        );
        const expected = referenceDerivative(name, scaledLanes)[lane][0];
        assertClose(
          result.outputs[lane][iteration],
          expected,
          `${caseName} | ${laneLabel(lane)} | component ${iteration} (iteration ${iteration}, x*${iteration + 1})`,
        );
      }
    }
    completedCases.push(caseName);
  }
}

function verifyInterleaveResult(caseName, result, expectedPerLane, expectedDiscarded) {
  assert.equal(result.outputs.length, 4, `${caseName} | outputs length`);
  assert.equal(result.discarded.length, 4, `${caseName} | discarded length`);
  assert.ok(result.errors.length === 0,
    `${caseName} | unexpected errors: ${result.errors.join("; ")}`);
  for (let lane = 0; lane < 4; ++lane) {
    assert.equal(result.discarded[lane], expectedDiscarded[lane],
      `${caseName} | ${laneLabel(lane)} | discard flag`);
    if (expectedDiscarded[lane]) {
      assert.equal(result.outputs[lane], null,
        `${caseName} | ${laneLabel(lane)} | discarded lane must not publish output`);
    } else {
      const expected = expectedPerLane[lane];
      assertLaneValues(caseName, "interleave", lane, result.outputs[lane], expected);
    }
  }
}

function testInterleavingAndStateIsolation() {
  // Shader A: lane-specific discard + Fine/Coarse derivatives.
  const quadA1 = [
    { 0: 1, 1: 1 },  // TL discards
    { 0: 4, 1: 0 },
    { 0: 8, 1: 0 },
    { 0: 20, 1: 0 },
  ];
  const quadA2 = [
    { 0: -3, 1: 0 },
    { 0: 6, 1: 0 },
    { 0: -9, 1: 0 },
    { 0: 15, 1: 1 }, // BR discards
  ];

  const expectedA1 = interleaveExpected(quadA1, true);
  const expectedA2 = interleaveExpected(quadA2, true);

  // Shader B: a different shader, run in between.
  const quadB = [
    { 0: [2, -3] },
    { 0: [5, 7] },
    { 0: [-4, 9] },
    { 0: [11, -6] },
  ];
  const laneB = quadB.map((lane) => lane[0]);
  const expectedB = [];
  for (let lane = 0; lane < 4; ++lane) {
    const dx = referenceDerivative("dpdxFine", laneB)[lane];
    const dy = referenceDerivative("dpdyFine", laneB)[lane];
    expectedB.push([dx[0], dx[1], dy[0], dy[1]]);
  }

  // Consecutive A runs with two different quads, an interleaved B run, then
  // the original A input restored.
  const a1 = debugFragmentQuad(INTERLEAVE_SHADER_A, "main", quadA1, {});
  verifyInterleaveResult("interleave/A-quad1/first", a1, expectedA1.laneValues, expectedA1.discarded);

  const a2 = debugFragmentQuad(INTERLEAVE_SHADER_A, "main", quadA2, {});
  verifyInterleaveResult("interleave/A-quad2", a2, expectedA2.laneValues, expectedA2.discarded);

  const b1 = debugFragmentQuad(INTERLEAVE_SHADER_B, "main", quadB, {});
  verifyInterleaveResult("interleave/B-quad", b1, expectedB, [false, false, false, false]);

  const a1Restored = debugFragmentQuad(INTERLEAVE_SHADER_A, "main", quadA1, {});
  verifyInterleaveResult("interleave/A-quad1/restored", a1Restored, expectedA1.laneValues, expectedA1.discarded);

  // Restoring the original input must reproduce the original result exactly
  // (no derivative/recursion cache or discard state leaked across runs).
  assert.deepStrictEqual(
    a1Restored.outputs,
    a1.outputs,
    "interleave | restored A outputs must equal the first A run",
  );
  assert.deepStrictEqual(
    a1Restored.discarded,
    a1.discarded,
    "interleave | restored A discard flags must equal the first A run",
  );
  assert.deepStrictEqual(
    a1Restored.errors,
    a1.errors,
    "interleave | restored A errors must equal the first A run",
  );
  completedCases.push("interleave/state-isolation");
}

// Expected outputs for INTERLEAVE_SHADER_A: vec4(dxFine, dyFine, fwidthFine, dxCoarse).
function interleaveExpected(quad, hasDiscard) {
  const scalarQuad = quad.map((lane) => [lane[0]]);
  const laneValues = [];
  for (let lane = 0; lane < 4; ++lane) {
    const dxf = referenceDerivative("dpdxFine", scalarQuad)[lane][0];
    const dyf = referenceDerivative("dpdyFine", scalarQuad)[lane][0];
    const fwf = referenceDerivative("fwidthFine", scalarQuad)[lane][0];
    const dxc = referenceDerivative("dpdxCoarse", scalarQuad)[lane][0];
    laneValues.push([dxf, dyf, fwf, dxc]);
  }
  const discarded = quad.map((lane) => hasDiscard && lane[1] > 0.5);
  return { laneValues, discarded };
}

function testDiscardBeforeAndAfterDerivative() {
  const quadBase = BASE_SCALAR_QUAD;
  for (let targetLane = 0; targetLane < 4; ++targetLane) {
    for (const discardAfterDerivative of [false, true]) {
      for (const name of DERIVATIVE_NAMES) {
        const mode = discardAfterDerivative ? "after" : "before";
        const caseName = `discard-${mode}-derivative/lane${targetLane}-${LANE_NAMES[targetLane]}/${name}`;
        const quad = quadBase.map((value, lane) => ({
          0: value,
          1: lane === targetLane ? 1 : 0, // only this lane discards
        }));
        const result = debugFragmentQuad(
          discardShader(name, discardAfterDerivative), "main", quad, {},
        );

        assert.equal(result.outputs.length, 4, `${caseName} | outputs length`);
        assert.equal(result.discarded.length, 4, `${caseName} | discarded length`);
        assert.ok(result.errors.length === 0,
          `${caseName} | unexpected errors: ${result.errors.join("; ")}`);

        // The discarded lane publishes nothing, but stays alive as a helper.
        assert.equal(result.discarded[targetLane], true,
          `${caseName} | ${laneLabel(targetLane)} | discard flag`);
        assert.equal(result.outputs[targetLane], null,
          `${caseName} | ${laneLabel(targetLane)} | discarded lane must not publish output`);

        // Every other lane runs the derivative under uniform control flow and
        // must see the discarded lane's input as a helper value.
        const expectedPerLane = padScalar(BASE_SCALAR_EXPECTED[name]);
        for (let lane = 0; lane < 4; ++lane) {
          if (lane === targetLane) continue;
          assert.equal(result.discarded[lane], false,
            `${caseName} | ${laneLabel(lane)} | should not be discarded`);
          assertLaneValues(caseName, name, lane, result.outputs[lane], expectedPerLane[lane]);
        }
        completedCases.push(caseName);
      }
    }
  }

  // Explicit spot checks from the specification: discarding TL must leave
  // BL's Fine dy = 7 (= BL - TL) and TR's Fine dy = 16 (= BR - TR).
  const beforeResult = debugFragmentQuad(
    discardShader("dpdyFine", false), "main",
    quadBase.map((value, lane) => ({ 0: value, 1: lane === 0 ? 1 : 0 })), {},
  );
  assertClose(beforeResult.outputs[2][0], 7,
    "discard-before-derivative | TL discarded | dpdyFine | lane 2 (BL) | component 0");
  assertClose(beforeResult.outputs[1][0], 16,
    "discard-before-derivative | TL discarded | dpdyFine | lane 1 (TR) | component 0");
}

// -----------------------------------------------------------------------------
// Entry point
// -----------------------------------------------------------------------------

function main() {
  const started = Date.now();
  testExplicitScalarBase();
  testTypesAndComponents();
  testSeededSignedData();
  testConstantTranslation();
  testInputScaling();
  testRowAndColumnSwap();
  testLoopWithChangingArguments();
  testDiscardBeforeAndAfterDerivative();
  testInterleavingAndStateIsolation();

  const elapsedMs = Date.now() - started;
  if (elapsedMs > 90_000) {
    throw new Error(`test suite exceeded the 90s budget (${elapsedMs} ms)`);
  }
  console.log(
    `quad_derivatives: OK - ${completedCases.length} cases, ` +
    `${comparisonCount} numeric comparisons, ${elapsedMs} ms, seed=0x${SEED.toString(16)}`,
  );
}

try {
  main();
} catch (error) {
  console.error(`quad_derivatives: FAIL\n${error && error.stack ? error.stack : error}`);
  process.exitCode = 1;
}
