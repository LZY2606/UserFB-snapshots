import test from "node:test";
import { strict as assert } from "node:assert";
import { Bezier } from "../src/bezier.js";

const EPS = 1e-8;

// ---------------------------------------------------------------------------
// Independent reference implementation.
//
// This deliberately uses a different quadrature scheme (adaptive Simpson)
// from the library's Gauss-Kronrod code, so agreement is not just the two new
// methods checking each other.
// ---------------------------------------------------------------------------

function referenceCoefficients(points) {
  if (points.length === 3) {
    const ax = 2 * (points[1].x - points[0].x),
      bx = 2 * (points[2].x - points[1].x),
      ay = 2 * (points[1].y - points[0].y),
      by = 2 * (points[2].y - points[1].y);
    return { x: [ax, bx - ax, 0], y: [ay, by - ay, 0] };
  }
  const dx = [
      3 * (points[1].x - points[0].x),
      3 * (points[2].x - points[1].x),
      3 * (points[3].x - points[2].x),
    ],
    dy = [
      3 * (points[1].y - points[0].y),
      3 * (points[2].y - points[1].y),
      3 * (points[3].y - points[2].y),
    ];
  return {
    x: [dx[0], 2 * (dx[1] - dx[0]), dx[0] - 2 * dx[1] + dx[2]],
    y: [dy[0], 2 * (dy[1] - dy[0]), dy[0] - 2 * dy[1] + dy[2]],
  };
}

function referenceSpeed(coefficients, t) {
  const dx =
      coefficients.x[0] +
      t * (coefficients.x[1] + t * coefficients.x[2]),
    dy =
      coefficients.y[0] +
      t * (coefficients.y[1] + t * coefficients.y[2]);
  return Math.hypot(dx, dy);
}

function referenceRoots(coefficients) {
  const a = coefficients[2],
    b = coefficients[1],
    c = coefficients[0];
  if (a === 0) {
    if (b === 0) return [];
    return [-c / b];
  }
  const discriminant = b * b - 4 * a * c;
  if (discriminant < 0) return [];
  if (discriminant === 0) return [-b / (2 * a)];
  const q = -0.5 * (b + (b < 0 ? -1 : 1) * Math.sqrt(discriminant));
  return [q / a, c / q];
}

function referenceCuts(coefficients, a, b) {
  const cuts = [a, b];
  for (const component of [coefficients.x, coefficients.y]) {
    for (const root of referenceRoots(component)) {
      if (Number.isFinite(root) && root > a && root < b) cuts.push(root);
    }
  }
  cuts.sort((u, v) => u - v);
  return cuts.filter(
    (value, index) => index === 0 || value !== cuts[index - 1]
  );
}

function simpson(f, a, b) {
  const mid = (a + b) / 2;
  return ((b - a) / 6) * (f(a) + 4 * f(mid) + f(b));
}

function adaptiveSimpson(f, a, b, epsilon, whole, depth = 0) {
  const mid = (a + b) / 2,
    left = simpson(f, a, mid),
    right = simpson(f, mid, b),
    delta = left + right - whole;
  if (depth >= 45 || Math.abs(delta) <= 15 * epsilon) {
    return left + right + delta / 15;
  }
  return (
    adaptiveSimpson(f, a, mid, epsilon / 2, left, depth + 1) +
    adaptiveSimpson(f, mid, b, epsilon / 2, right, depth + 1)
  );
}

function referenceLength(points, a = 0, b = 1, epsilon = 1e-12) {
  if (b <= a) return 0;
  const coefficients = referenceCoefficients(points),
    cuts = referenceCuts(coefficients, a, b),
    segments = cuts.length - 1;
  let total = 0;
  for (let i = 0; i < segments; i++) {
    const f = (t) => referenceSpeed(coefficients, t);
    total += adaptiveSimpson(
      f,
      cuts[i],
      cuts[i + 1],
      epsilon / segments,
      simpson(f, cuts[i], cuts[i + 1])
    );
  }
  return total;
}

// Analytic arc length for (0,0), (1,2), (2,2), (3,0):
// |B'(t)| = 3 * sqrt(1 + 4(1-2t)^2).
function analyticSmoothLength(scale, t) {
  const antiderivative = (v) =>
    (v / 2) * Math.sqrt(1 + 4 * v * v) + (1 / 4) * Math.asinh(2 * v);
  return scale * 1.5 * (antiderivative(1) - antiderivative(1 - 2 * t));
}

// Analytic arc length for the folding quadratic (0,0), (2,0), (0,0):
// |B'(t)| = 4*|1-2t|.
function analyticFoldQuadraticLength(t) {
  return t <= 0.5 ? 4 * (t - t * t) : 2 + 4 * t * t - 4 * t;
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

test(
  "ordinary quadratic and cubic curves match an independent reference",
  () => {
  const curves = [
    {
      points: [
        { x: 0, y: 0 },
        { x: 1, y: 2 },
        { x: 2, y: -1 },
      ],
    },
    {
      points: [
        { x: 0, y: 0 },
        { x: 0.5, y: 2 },
        { x: 2, y: -0.5 },
        { x: 2.5, y: 1 },
      ],
    },
  ];

  for (const { points } of curves) {
    const curve = new Bezier(points),
      total = referenceLength(points, 0, 1);

    assert.ok(Math.abs(curve.lengthAt(0) - 0) <= EPS);
    assert.ok(Math.abs(curve.lengthAt(1) - total) <= EPS);

    let previous = 0;
    for (const t of [0.13, 0.26, 0.41, 0.58, 0.73, 0.88, 0.99]) {
      const got = curve.lengthAt(t),
        expected = referenceLength(points, 0, t);
      assert.ok(Math.abs(got - expected) <= EPS, `lengthAt(${t})`);
      assert.ok(got >= previous, "arc length is monotone");
      previous = got;
    }

    for (const fraction of [0.17, 0.35, 0.52, 0.71, 0.93]) {
      const distance = total * fraction,
        t = curve.parameterAtLength(distance);
      assert.ok(t >= 0 && t <= 1);
      assert.ok(
        Math.abs(referenceLength(points, 0, t) - distance) <= EPS,
        `parameterAtLength(${distance})`
      );
    }
  }
});

test("a coordinate-level tolerance is honored for both methods", () => {
  const points = [
    { x: 0, y: 0 },
    { x: 0.5, y: 2 },
    { x: 2, y: -0.5 },
    { x: 2.5, y: 1 },
  ];
  const curve = new Bezier(points),
    total = referenceLength(points, 0, 1),
    tolerance = 1e-3;

  for (const t of [0.3, 0.6, 1]) {
    assert.ok(
      Math.abs(curve.lengthAt(t, tolerance) - referenceLength(points, 0, t)) <=
        tolerance
    );
  }
  const distance = total * 0.42,
    t = curve.parameterAtLength(distance, tolerance);
  assert.ok(Math.abs(referenceLength(points, 0, t) - distance) <= tolerance);
});

test("folding quadratic (0,0),(2,0),(0,0) uses true arc length", () => {
  const points = [
    { x: 0, y: 0 },
    { x: 2, y: 0 },
    { x: 0, y: 0 },
  ];
  const curve = new Bezier(points);

  assert.ok(Math.abs(curve.lengthAt(0.5) - 1) <= EPS);
  assert.ok(Math.abs(curve.lengthAt(1) - 2) <= EPS);
  assert.ok(
    Math.abs(curve.lengthAt(0.25) - analyticFoldQuadraticLength(0.25)) <=
      EPS
  );
  assert.ok(
    Math.abs(curve.lengthAt(0.75) - analyticFoldQuadraticLength(0.75)) <=
      EPS
  );

  // The endpoints coincide, but the curve is not degenerate.
  assert.ok(curve.lengthAt(1) > 0);

  // distance 1 is exactly the stationary point, distance 1.5 is beyond it.
  const tAtOne = curve.parameterAtLength(1);
  assert.ok(Math.abs(tAtOne - 0.5) <= 1e-6);

  const expected = (1 + Math.sqrt(0.5)) / 2,
    tAtOnePointFive = curve.parameterAtLength(1.5);
  assert.ok(
    Math.abs(analyticFoldQuadraticLength(tAtOnePointFive) - 1.5) <= EPS
  );
  assert.ok(Math.abs(tAtOnePointFive - expected) <= 1e-6);

  // No "distance / total" shortcut: distance 1.5 is not t=0.75.
  assert.ok(Math.abs(tAtOnePointFive - 0.75) > 1e-3);

  assert.equal(curve.parameterAtLength(2.1), 1);
  assert.ok(Math.abs(curve.parameterAtLength(2) - 1) <= 1e-6);
});

test(
  "folding cubic (0,0),(2,0),(-1,0),(1,0) handles two stationary points",
  () => {
  const points = [
    { x: 0, y: 0 },
    { x: 2, y: 0 },
    { x: -1, y: 0 },
    { x: 1, y: 0 },
  ];
  const curve = new Bezier(points),
    root5 = Math.sqrt(5),
    t1 = (1 - 1 / root5) / 2,
    t2 = (1 + 1 / root5) / 2,
    total = 1 + 2 / root5;

  assert.ok(Math.abs(curve.lengthAt(1) - total) <= EPS);
  // Arc length at the first stationary point is the x coordinate it reaches.
  // The second stationary point is the far turn of the curve.
  assert.ok(Math.abs(curve.lengthAt(t1) - t2) <= EPS);
  assert.ok(Math.abs(curve.lengthAt(t2) - (2 * t2 - t1)) <= EPS);

  // These distances land exactly on stationary points, where speed is zero.
  assert.ok(Math.abs(curve.parameterAtLength(t2) - t1) <= 1e-6);
  assert.ok(Math.abs(curve.parameterAtLength(2 * t2 - t1) - t2) <= 1e-6);

  // Nothing may produce NaN around the zero-speed points.
  for (const t of [t1, t2]) {
    for (const offset of [-1e-9, 0, 1e-9]) {
      assert.ok(Number.isFinite(curve.lengthAt(t + offset)));
    }
    const parameter = curve.parameterAtLength(curve.lengthAt(t));
    assert.ok(Number.isFinite(parameter));
  }

  assert.equal(curve.parameterAtLength(total + 0.5), 1);
});

test("fully degenerate curves return zero for every valid distance", () => {
  const quadratics = [
    { points: [{ x: 3, y: 4 }, { x: 3, y: 4 }, { x: 3, y: 4 }] },
    {
      points: [
        { x: -2, y: 0 },
        { x: -2, y: 0 },
        { x: -2, y: 0 },
        { x: -2, y: 0 },
      ],
    },
  ];

  for (const { points } of quadratics) {
    const curve = new Bezier(points);
    assert.equal(curve.lengthAt(0), 0);
    assert.equal(curve.lengthAt(0.5), 0);
    assert.equal(curve.lengthAt(1), 0);
    for (const distance of [0, 1, 1e6]) {
      assert.equal(curve.parameterAtLength(distance), 0);
    }
  }
});

test("parameterAtLength reverses correctly on a reversed curve", () => {
  const points = [
    { x: 0, y: 0 },
    { x: 0.5, y: 2 },
    { x: 2, y: -0.5 },
    { x: 2.5, y: 1 },
  ];
  const reversed = [...points].reverse(),
    forward = new Bezier(points),
    backward = new Bezier(reversed),
    total = referenceLength(points, 0, 1);

  for (const fraction of [0.12, 0.37, 0.59, 0.81]) {
    const distance = total * fraction,
      t = forward.parameterAtLength(distance),
      tReversed = backward.parameterAtLength(total - distance);
    assert.ok(
      Math.abs(tReversed - (1 - t)) <= 1e-7,
      `reverse complement at ${distance}`
    );
  }
});

test(
  "arc-length accuracy survives a 1000x and near-1e6 coordinate scale",
  () => {
  for (const scale of [1000, 300000]) {
    const points = [
      { x: 0, y: 0 },
      { x: scale, y: 2 * scale },
      { x: 2 * scale, y: 2 * scale },
      { x: 3 * scale, y: 0 },
    ];
    const curve = new Bezier(points),
      total = analyticSmoothLength(scale, 1);

    for (const t of [0.2, 0.5, 0.8, 1]) {
      assert.ok(
        Math.abs(curve.lengthAt(t) - analyticSmoothLength(scale, t)) <= EPS,
        `scaled lengthAt(${t}) at ${scale}`
      );
    }

    for (const fraction of [0.17, 0.5, 0.83]) {
      const distance = total * fraction,
        t = curve.parameterAtLength(distance);
      assert.ok(
        Math.abs(analyticSmoothLength(scale, t) - distance) <= EPS,
        `scaled parameterAtLength(${distance}) at ${scale}`
      );
    }
  }
});

test("queries do not mutate points, ratios, or the visible LUT", () => {
  const curve = new Bezier(0, 0, 0.5, 2, 2, -0.5, 2.5, 1);

  curve.getLUT(10);
  const pointsBefore = JSON.stringify(curve.points),
    ratiosBefore = curve.ratios,
    lutBefore = JSON.stringify(curve.getLUT(10));

  curve.lengthAt(0.3);
  curve.lengthAt(0.7, 1e-3);
  curve.parameterAtLength(0.5);
  curve.parameterAtLength(curve.lengthAt(1) * 0.6, 1e-3);

  assert.equal(JSON.stringify(curve.points), pointsBefore);
  assert.equal(curve.ratios, ratiosBefore);
  assert.equal(JSON.stringify(curve.getLUT(10)), lutBefore);
});

test(
  "editing a control point and calling update() invalidates old results",
  () => {
  const curve = new Bezier(0, 0, 0.5, 2, 2, -0.5, 2.5, 1);
  const before = curve.lengthAt(0.5);

  curve.points[1] = { x: 0.1, y: 3.4 };
  curve.update();

  const newPoints = curve.points.map((p) => ({ x: p.x, y: p.y }));
  const after = curve.lengthAt(0.5);

  assert.ok(Math.abs(after - referenceLength(newPoints, 0, 0.5)) <= EPS);
  assert.ok(
    Math.abs(after - new Bezier(newPoints).lengthAt(0.5)) <= EPS,
    "the new points must be what is integrated, not a cached table"
  );

  const newTotal = referenceLength(newPoints, 0, 1);
  const distance = newTotal * 0.43,
    t = curve.parameterAtLength(distance);
  assert.ok(Math.abs(referenceLength(newPoints, 0, t) - distance) <= EPS);

  assert.ok(Math.abs(after - before) > 1e-6, "the edit changed the curve");
  assert.equal(curve.points[1].x, 0.1);
  assert.equal(curve.points[1].y, 3.4);
});

test("invalid arguments throw RangeError", () => {
  const curve = new Bezier(0, 0, 1, 2, 2, -1);

  for (const t of [-1, 1.5, NaN, Infinity, -Infinity, "0.5", null, undefined]) {
    assert.throws(() => curve.lengthAt(t), RangeError, `t=${String(t)}`);
  }

  for (const distance of [
    -1,
    NaN,
    Infinity,
    -Infinity,
    "1",
    null,
    undefined,
  ]) {
    assert.throws(
      () => curve.parameterAtLength(distance),
      RangeError,
      `distance=${String(distance)}`
    );
  }

  for (const tolerance of [0, -1, NaN, Infinity, -Infinity, "1e-8", null]) {
    assert.throws(() => curve.lengthAt(0.5, tolerance), RangeError);
    assert.throws(() => curve.parameterAtLength(1, tolerance), RangeError);
  }

  // The documented defaults and supported tolerance range are accepted.
  assert.ok(Number.isFinite(curve.lengthAt(0.5)));
  assert.ok(Number.isFinite(curve.parameterAtLength(1)));
  assert.ok(Number.isFinite(curve.lengthAt(0.5, 1e-8)));
  assert.ok(Number.isFinite(curve.parameterAtLength(1, 1e-3)));
});
