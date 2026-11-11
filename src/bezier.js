/**
  A javascript Bezier curve library by Pomax.

  Based on http://pomax.github.io/bezierinfo

  This code is MIT licensed.
**/

import { utils } from "./utils.js";
import { PolyBezier } from "./poly-bezier.js";

// math-inlining.
const { abs, min, max, cos, sin, acos, sqrt } = Math;
const pi = Math.PI;
// a zero coordinate, which is surprisingly useful
const ZERO = { x: 0, y: 0, z: 0 };

// ---------------------------------------------------------------------------
// Arc-length helpers for `lengthAt` / `parameterAtLength`.
//
// The derivative of a 2D quadratic or cubic Bezier is a polynomial of degree
// at most two, and its components only have a handful of roots. Splitting the
// integration interval at those roots means the (possibly zero) speed has no
// sign change or cusp inside an interval, which is what makes the adaptive
// quadrature both accurate and safe around stationary points.
// ---------------------------------------------------------------------------

function finiteNumber(value) {
  return typeof value === "number" && Number.isFinite(value);
}

function derivativeCoefficients(points) {
  if (points.length === 3) {
    // quadratic: B'(t) = (1-t)*d0 + t*d1, so it is linear.
    const dx0 = 2 * (points[1].x - points[0].x),
      dy0 = 2 * (points[1].y - points[0].y),
      dx1 = 2 * (points[2].x - points[1].x),
      dy1 = 2 * (points[2].y - points[1].y);
    return {
      x: [dx0, dx1 - dx0, 0],
      y: [dy0, dy1 - dy0, 0],
    };
  }
  // cubic: B'(t) is quadratic, built from the three derivative control points.
  const dx0 = 3 * (points[1].x - points[0].x),
    dy0 = 3 * (points[1].y - points[0].y),
    dx1 = 3 * (points[2].x - points[1].x),
    dy1 = 3 * (points[2].y - points[1].y),
    dx2 = 3 * (points[3].x - points[2].x),
    dy2 = 3 * (points[3].y - points[2].y);
  // coefficients for c0 + c1*t + c2*t^2
  return {
    x: [dx0, 2 * (dx1 - dx0), dx0 - 2 * dx1 + dx2],
    y: [dy0, 2 * (dy1 - dy0), dy0 - 2 * dy1 + dy2],
  };
}

function polynomialValue(coefficients, t) {
  return coefficients[0] + t * (coefficients[1] + t * coefficients[2]);
}

function derivativeSpeed(coefficients, t) {
  const dx = polynomialValue(coefficients.x, t);
  const dy = polynomialValue(coefficients.y, t);
  return sqrt(dx * dx + dy * dy);
}

function componentRoots(coefficients) {
  const a = coefficients[2],
    b = coefficients[1],
    c = coefficients[0];
  if (a === 0) {
    if (b === 0) return []; // constant (possibly zero) component.
    return [-c / b];
  }
  const discriminant = b * b - 4 * a * c;
  if (discriminant < 0) return [];
  if (discriminant === 0) return [-b / (2 * a)];
  // Numerically stable quadratic roots.
  const q = -0.5 * (b + (b < 0 ? -1 : 1) * sqrt(discriminant));
  return [q / a, c / q];
}

function arcLengthCuts(coefficients, a, b) {
  const cuts = [a, b];
  [coefficients.x, coefficients.y].forEach((component) => {
    componentRoots(component).forEach((root) => {
      if (finiteNumber(root) && root > a && root < b) cuts.push(root);
    });
  });
  cuts.sort((u, v) => u - v);
  // Only drop exact duplicates. Roots that are merely close together still
  // split the interval, so a stationary point can never end up inside a
  // segment where the speed is treated as smooth.
  return cuts.filter(
    (value, index) => index === 0 || value !== cuts[index - 1]
  );
}

// Gauss-Kronrod (15 point, with the embedded 7 point Gauss rule).
const GK15_X = [
  0.991455371120812639206854697526329, 0.949107912342758524526189684047851,
  0.864864423359769072789712788640926, 0.741531185599394439863864773280788,
  0.586087235467691130294144838258730, 0.405845151377397166906606412076961,
  0.207784955007898467600689403773245,
];
const GK15_W = [
  0.022935322010529224963732008058970, 0.063092092629978553290700663189204,
  0.104790010322250183839876322541518, 0.140653259715525918745189590510238,
  0.169004726639267902826583426598550, 0.190350578064785409913256402421014,
  0.204432940075298892414161999234649, 0.209482141084727828012999174891714,
];
const G7_W = [
  0.129484966168869693270611432679082, 0.279705391489276667901467771423780,
  0.381830050505118944950369775488975, 0.417959183673469387755102040816327,
];
const ARC_LENGTH_MAX_DEPTH = 50;
const ARC_LENGTH_EVALUATION_BUDGET = 30000;

function gaussKronrod15(f, a, b, tolerance, depth, budget) {
  const center = (a + b) / 2,
    half = (b - a) / 2,
    fc = f(center);
  let kronrod = GK15_W[7] * fc,
    gauss = G7_W[3] * fc;
  budget.remaining -= 15;

  for (let i = 0; i < 7; i++) {
    const offset = half * GK15_X[i],
      f1 = f(center - offset),
      f2 = f(center + offset);
    kronrod += GK15_W[i] * (f1 + f2);
    if (i % 2 === 1) gauss += G7_W[(i - 1) / 2] * (f1 + f2);
  }
  kronrod *= half;
  gauss *= half;

  if (
    abs(kronrod - gauss) <= tolerance ||
    depth >= ARC_LENGTH_MAX_DEPTH ||
    budget.remaining <= 0
  ) {
    return kronrod;
  }
  const mid = center;
  return (
    gaussKronrod15(f, a, mid, tolerance / 2, depth + 1, budget) +
    gaussKronrod15(f, mid, b, tolerance / 2, depth + 1, budget)
  );
}

function arcLengthBetween(coefficients, a, b, tolerance) {
  if (!(b > a)) return 0;
  const cuts = arcLengthCuts(coefficients, a, b),
    segments = cuts.length - 1,
    segmentTolerance = tolerance / segments;
  // Kahan summation keeps large-coordinate results from accumulating avoidable
  // round-off across segments.
  let sum = 0,
    correction = 0;
  const budget = { remaining: ARC_LENGTH_EVALUATION_BUDGET };
  for (let i = 0; i < segments; i++) {
    const value = gaussKronrod15(
      (t) => derivativeSpeed(coefficients, t),
      cuts[i],
      cuts[i + 1],
      segmentTolerance,
      0,
      budget
    );
    const adjusted = value - correction,
      next = sum + adjusted;
    correction = next - sum - adjusted;
    sum = next;
  }
  return sum;
}

function arcLengthTable(coefficients, tolerance) {
  const cuts = arcLengthCuts(coefficients, 0, 1),
    segments = cuts.length - 1,
    segmentTolerance = tolerance / segments,
    lengths = [0];
  let sum = 0,
    correction = 0;
  const budget = { remaining: ARC_LENGTH_EVALUATION_BUDGET };
  for (let i = 0; i < segments; i++) {
    const value = gaussKronrod15(
      (t) => derivativeSpeed(coefficients, t),
      cuts[i],
      cuts[i + 1],
      segmentTolerance,
      0,
      budget
    );
    const adjusted = value - correction,
      next = sum + adjusted;
    correction = next - sum - adjusted;
    sum = next;
    lengths.push(sum);
  }
  return { cuts, lengths };
}

function hasZeroDerivative(coefficients) {
  return [coefficients.x, coefficients.y].every(
    (component) =>
      component[0] === 0 && component[1] === 0 && component[2] === 0
  );
}

/**
 * Bezier curve constructor.
 *
 * ...docs pending...
 */
class Bezier {
  constructor(coords) {
    let args =
      coords && coords.forEach ? coords : Array.from(arguments).slice();
    let coordlen = false;

    if (typeof args[0] === "object") {
      coordlen = args.length;
      const newargs = [];
      args.forEach(function (point) {
        ["x", "y", "z"].forEach(function (d) {
          if (typeof point[d] !== "undefined") {
            newargs.push(point[d]);
          }
        });
      });
      args = newargs;
    }

    let higher = false;
    const len = args.length;

    if (coordlen) {
      if (coordlen > 4) {
        if (arguments.length !== 1) {
          throw new Error(
            "Only new Bezier(point[]) is accepted for 4th and higher order curves"
          );
        }
        higher = true;
      }
    } else {
      if (len !== 6 && len !== 8 && len !== 9 && len !== 12) {
        if (arguments.length !== 1) {
          throw new Error(
            "Only new Bezier(point[]) is accepted for 4th and higher order curves"
          );
        }
      }
    }

    const _3d = (this._3d =
      (!higher && (len === 9 || len === 12)) ||
      (coords && coords[0] && typeof coords[0].z !== "undefined"));

    const points = (this.points = []);
    for (let idx = 0, step = _3d ? 3 : 2; idx < len; idx += step) {
      var point = {
        x: args[idx],
        y: args[idx + 1],
      };
      if (_3d) {
        point.z = args[idx + 2];
      }
      points.push(point);
    }
    const order = (this.order = points.length - 1);

    const dims = (this.dims = ["x", "y"]);
    if (_3d) dims.push("z");
    this.dimlen = dims.length;

    // is this curve, practically speaking, a straight line?
    const aligned = utils.align(points, { p1: points[0], p2: points[order] });
    const baselength = utils.dist(points[0], points[order]);
    this._linear = aligned.reduce((t, p) => t + abs(p.y), 0) < baselength / 50;

    this._lut = [];
    this._t1 = 0;
    this._t2 = 1;
    this.update();
  }

  static quadraticFromPoints(p1, p2, p3, t) {
    if (typeof t === "undefined") {
      t = 0.5;
    }
    // shortcuts, although they're really dumb
    if (t === 0) {
      return new Bezier(p2, p2, p3);
    }
    if (t === 1) {
      return new Bezier(p1, p2, p2);
    }
    // real fitting.
    const abc = Bezier.getABC(2, p1, p2, p3, t);
    return new Bezier(p1, abc.A, p3);
  }

  static cubicFromPoints(S, B, E, t, d1) {
    if (typeof t === "undefined") {
      t = 0.5;
    }
    const abc = Bezier.getABC(3, S, B, E, t);
    if (typeof d1 === "undefined") {
      d1 = utils.dist(B, abc.C);
    }
    const d2 = (d1 * (1 - t)) / t;

    const selen = utils.dist(S, E),
      lx = (E.x - S.x) / selen,
      ly = (E.y - S.y) / selen,
      bx1 = d1 * lx,
      by1 = d1 * ly,
      bx2 = d2 * lx,
      by2 = d2 * ly;
    // derivation of new hull coordinates
    const e1 = { x: B.x - bx1, y: B.y - by1 },
      e2 = { x: B.x + bx2, y: B.y + by2 },
      A = abc.A,
      v1 = { x: A.x + (e1.x - A.x) / (1 - t), y: A.y + (e1.y - A.y) / (1 - t) },
      v2 = { x: A.x + (e2.x - A.x) / t, y: A.y + (e2.y - A.y) / t },
      nc1 = { x: S.x + (v1.x - S.x) / t, y: S.y + (v1.y - S.y) / t },
      nc2 = {
        x: E.x + (v2.x - E.x) / (1 - t),
        y: E.y + (v2.y - E.y) / (1 - t),
      };
    // ...done
    return new Bezier(S, nc1, nc2, E);
  }

  static getUtils() {
    return utils;
  }

  getUtils() {
    return Bezier.getUtils();
  }

  static get PolyBezier() {
    return PolyBezier;
  }

  valueOf() {
    return this.toString();
  }

  toString() {
    return utils.pointsToString(this.points);
  }

  toSVG() {
    if (this._3d) return false;
    const p = this.points,
      x = p[0].x,
      y = p[0].y,
      s = ["M", x, y, this.order === 2 ? "Q" : "C"];
    for (let i = 1, last = p.length; i < last; i++) {
      s.push(p[i].x);
      s.push(p[i].y);
    }
    return s.join(" ");
  }

  setRatios(ratios) {
    if (ratios.length !== this.points.length) {
      throw new Error("incorrect number of ratio values");
    }
    this.ratios = ratios;
    this._lut = []; //  invalidate any precomputed LUT
  }

  verify() {
    const print = this.coordDigest();
    if (print !== this._print) {
      this._print = print;
      this.update();
    }
  }

  coordDigest() {
    return this.points
      .map(function (c, pos) {
        return "" + pos + c.x + c.y + (c.z ? c.z : 0);
      })
      .join("");
  }

  update() {
    // invalidate any precomputed LUT
    this._lut = [];
    this.dpoints = utils.derive(this.points, this._3d);
    this.computedirection();
  }

  computedirection() {
    const points = this.points;
    const angle = utils.angle(points[0], points[this.order], points[1]);
    this.clockwise = angle > 0;
  }

  length() {
    return utils.length(this.derivative.bind(this));
  }

  _arcLengthCoefficients() {
    if (this._3d) {
      throw new Error("lengthAt/parameterAtLength only support 2D curves");
    }
    if (this.ratios) {
      throw new Error(
        "lengthAt/parameterAtLength do not support rational curves"
      );
    }
    if (this.order !== 2 && this.order !== 3) {
      throw new Error(
        "lengthAt/parameterAtLength only support quadratic and cubic curves"
      );
    }
    for (let i = 0; i < this.points.length; i++) {
      if (
        !Number.isFinite(this.points[i].x) ||
        !Number.isFinite(this.points[i].y)
      ) {
        throw new Error(
          "lengthAt/parameterAtLength require finite control points"
        );
      }
    }
    return derivativeCoefficients(this.points);
  }

  /**
   * Returns the arc length from t=0 to `t`.
   *
   * Only 2D quadratic and cubic (non-rational) curves are supported. The
   * result has an absolute error of at most `tolerance`, measured in curve
   * coordinates.
   */
  lengthAt(t, tolerance = 1e-8) {
    if (!finiteNumber(t) || t < 0 || t > 1) {
      throw new RangeError("t must be a finite number in [0,1]");
    }
    if (!finiteNumber(tolerance) || tolerance <= 0) {
      throw new RangeError(
        "tolerance must be a finite number greater than zero"
      );
    }
    const coefficients = this._arcLengthCoefficients();
    if (t === 0 || hasZeroDerivative(coefficients)) return 0;
    // Keep integration comfortably inside the promised absolute error.
    return arcLengthBetween(coefficients, 0, t, tolerance / 2);
  }

  /**
   * Returns the parameter `t` at which the arc length from the start of the
   * curve equals `distance`.
   *
   * Distances past the end of the curve return 1, and a fully degenerate
   * curve returns 0. The arc length at the returned parameter differs from
   * `distance` by at most `tolerance`, measured in curve coordinates.
   */
  parameterAtLength(distance, tolerance = 1e-8) {
    if (!finiteNumber(distance) || distance < 0) {
      throw new RangeError(
        "distance must be a finite, non-negative number"
      );
    }
    if (!finiteNumber(tolerance) || tolerance <= 0) {
      throw new RangeError(
        "tolerance must be a finite number greater than zero"
      );
    }
    const coefficients = this._arcLengthCoefficients();
    if (hasZeroDerivative(coefficients)) return 0;
    if (distance === 0) return 0;

    const { cuts, lengths } = arcLengthTable(coefficients, tolerance / 16);
    const total = lengths[lengths.length - 1];
    if (distance >= total) return 1;

    // A target that lands on a stationary point (a breakpoint between
    // monotone pieces) has no meaningful parameter precision beyond the
    // requested arc-length residual. Returning the breakpoint itself both
    // satisfies that residual and avoids the ill-conditioned inversion there.
    for (let i = 1; i < cuts.length - 1; i++) {
      if (abs(distance - lengths[i]) <= tolerance / 2) return cuts[i];
    }

    // Find the monotone piece containing the target.
    let segment = 0;
    while (segment < cuts.length - 2 && distance > lengths[segment + 1]) {
      segment++;
    }
    const start = cuts[segment],
      end = cuts[segment + 1],
      base = lengths[segment],
      target = distance - base;

    // Bisection on arc length. The loop is capped and also stops as soon as
    // no representable midpoint remains, so it terminates even at stationary
    // points. Bisecting all the way to the parameter limit (rather than
    // stopping as soon as the arc-length bracket is within tolerance) also
    // gives a parameter-accurate answer where the speed is non-zero.
    const evaluationTolerance = tolerance / 1024;
    let lo = start,
      hi = end;

    for (let i = 0; i < 80; i++) {
      const mid = (lo + hi) / 2;
      if (mid === lo || mid === hi) break;
      const lengthMid = arcLengthBetween(
        coefficients,
        start,
        mid,
        evaluationTolerance
      );
      if (lengthMid < target) {
        lo = mid;
      } else {
        hi = mid;
      }
    }
    return (lo + hi) / 2;
  }

  static getABC(order = 2, S, B, E, t = 0.5) {
    const u = utils.projectionratio(t, order),
      um = 1 - u,
      C = {
        x: u * S.x + um * E.x,
        y: u * S.y + um * E.y,
      },
      s = utils.abcratio(t, order),
      A = {
        x: B.x + (B.x - C.x) / s,
        y: B.y + (B.y - C.y) / s,
      };
    return { A, B, C, S, E };
  }

  getABC(t, B) {
    B = B || this.get(t);
    let S = this.points[0];
    let E = this.points[this.order];
    return Bezier.getABC(this.order, S, B, E, t);
  }

  getLUT(steps) {
    this.verify();
    steps = steps || 100;
    if (this._lut.length === steps + 1) {
      return this._lut;
    }
    this._lut = [];
    // n steps means n+1 points
    steps++;
    this._lut = [];
    for (let i = 0, p, t; i < steps; i++) {
      t = i / (steps - 1);
      p = this.compute(t);
      p.t = t;
      this._lut.push(p);
    }
    return this._lut;
  }

  on(point, error) {
    error = error || 5;
    const lut = this.getLUT(),
      hits = [];
    for (let i = 0, c, t = 0; i < lut.length; i++) {
      c = lut[i];
      if (utils.dist(c, point) < error) {
        hits.push(c);
        t += i / lut.length;
      }
    }
    if (!hits.length) return false;
    return (t /= hits.length);
  }

  project(point) {
    // step 1: coarse check
    const LUT = this.getLUT(),
      l = LUT.length - 1,
      closest = utils.closest(LUT, point),
      mpos = closest.mpos,
      t1 = (mpos - 1) / l,
      t2 = (mpos + 1) / l,
      step = 0.1 / l;

    // step 2: fine check
    let mdist = closest.mdist,
      t = t1,
      ft = t,
      p;
    mdist += 1;
    for (let d; t < t2 + step; t += step) {
      p = this.compute(t);
      d = utils.dist(point, p);
      if (d < mdist) {
        mdist = d;
        ft = t;
      }
    }
    ft = ft < 0 ? 0 : ft > 1 ? 1 : ft;
    p = this.compute(ft);
    p.t = ft;
    p.d = mdist;
    return p;
  }

  get(t) {
    return this.compute(t);
  }

  point(idx) {
    return this.points[idx];
  }

  compute(t) {
    if (this.ratios) {
      return utils.computeWithRatios(t, this.points, this.ratios, this._3d);
    }
    return utils.compute(t, this.points, this._3d, this.ratios);
  }

  raise() {
    const p = this.points,
      np = [p[0]],
      k = p.length;
    for (let i = 1, pi, pim; i < k; i++) {
      pi = p[i];
      pim = p[i - 1];
      np[i] = {
        x: ((k - i) / k) * pi.x + (i / k) * pim.x,
        y: ((k - i) / k) * pi.y + (i / k) * pim.y,
      };
    }
    np[k] = p[k - 1];
    return new Bezier(np);
  }

  derivative(t) {
    return utils.compute(t, this.dpoints[0], this._3d);
  }

  dderivative(t) {
    return utils.compute(t, this.dpoints[1], this._3d);
  }

  align() {
    let p = this.points;
    return new Bezier(utils.align(p, { p1: p[0], p2: p[p.length - 1] }));
  }

  curvature(t) {
    return utils.curvature(t, this.dpoints[0], this.dpoints[1], this._3d);
  }

  inflections() {
    return utils.inflections(this.points);
  }

  normal(t) {
    return this._3d ? this.__normal3(t) : this.__normal2(t);
  }

  __normal2(t) {
    const d = this.derivative(t);
    const q = sqrt(d.x * d.x + d.y * d.y);
    return { t, x: -d.y / q, y: d.x / q };
  }

  __normal3(t) {
    // see http://stackoverflow.com/questions/25453159
    const r1 = this.derivative(t),
      r2 = this.derivative(t + 0.01),
      q1 = sqrt(r1.x * r1.x + r1.y * r1.y + r1.z * r1.z),
      q2 = sqrt(r2.x * r2.x + r2.y * r2.y + r2.z * r2.z);
    r1.x /= q1;
    r1.y /= q1;
    r1.z /= q1;
    r2.x /= q2;
    r2.y /= q2;
    r2.z /= q2;
    // cross product
    const c = {
      x: r2.y * r1.z - r2.z * r1.y,
      y: r2.z * r1.x - r2.x * r1.z,
      z: r2.x * r1.y - r2.y * r1.x,
    };
    const m = sqrt(c.x * c.x + c.y * c.y + c.z * c.z);
    c.x /= m;
    c.y /= m;
    c.z /= m;
    // rotation matrix
    const R = [
      c.x * c.x,
      c.x * c.y - c.z,
      c.x * c.z + c.y,
      c.x * c.y + c.z,
      c.y * c.y,
      c.y * c.z - c.x,
      c.x * c.z - c.y,
      c.y * c.z + c.x,
      c.z * c.z,
    ];
    // normal vector:
    const n = {
      t,
      x: R[0] * r1.x + R[1] * r1.y + R[2] * r1.z,
      y: R[3] * r1.x + R[4] * r1.y + R[5] * r1.z,
      z: R[6] * r1.x + R[7] * r1.y + R[8] * r1.z,
    };
    return n;
  }

  hull(t) {
    let p = this.points,
      _p = [],
      q = [],
      idx = 0;
    q[idx++] = p[0];
    q[idx++] = p[1];
    q[idx++] = p[2];
    if (this.order === 3) {
      q[idx++] = p[3];
    }
    // we lerp between all points at each iteration, until we have 1 point left.
    while (p.length > 1) {
      _p = [];
      for (let i = 0, pt, l = p.length - 1; i < l; i++) {
        pt = utils.lerp(t, p[i], p[i + 1]);
        q[idx++] = pt;
        _p.push(pt);
      }
      p = _p;
    }
    return q;
  }

  split(t1, t2) {
    // shortcuts
    if (t1 === 0 && !!t2) {
      return this.split(t2).left;
    }
    if (t2 === 1) {
      return this.split(t1).right;
    }

    // no shortcut: use "de Casteljau" iteration.
    const q = this.hull(t1);
    const result = {
      left:
        this.order === 2
          ? new Bezier([q[0], q[3], q[5]])
          : new Bezier([q[0], q[4], q[7], q[9]]),
      right:
        this.order === 2
          ? new Bezier([q[5], q[4], q[2]])
          : new Bezier([q[9], q[8], q[6], q[3]]),
      span: q,
    };

    // make sure we bind _t1/_t2 information!
    result.left._t1 = utils.map(0, 0, 1, this._t1, this._t2);
    result.left._t2 = utils.map(t1, 0, 1, this._t1, this._t2);
    result.right._t1 = utils.map(t1, 0, 1, this._t1, this._t2);
    result.right._t2 = utils.map(1, 0, 1, this._t1, this._t2);

    // if we have no t2, we're done
    if (!t2) {
      return result;
    }

    // if we have a t2, split again:
    t2 = utils.map(t2, t1, 1, 0, 1);
    return result.right.split(t2).left;
  }

  extrema() {
    const result = {};
    let roots = [];

    this.dims.forEach(
      function (dim) {
        let mfn = function (v) {
          return v[dim];
        };
        let p = this.dpoints[0].map(mfn);
        result[dim] = utils.droots(p);
        if (this.order === 3) {
          p = this.dpoints[1].map(mfn);
          result[dim] = result[dim].concat(utils.droots(p));
        }
        result[dim] = result[dim].filter(function (t) {
          return t >= 0 && t <= 1;
        });
        roots = roots.concat(result[dim].sort(utils.numberSort));
      }.bind(this)
    );

    result.values = roots.sort(utils.numberSort).filter(function (v, idx) {
      return roots.indexOf(v) === idx;
    });

    return result;
  }

  bbox() {
    const extrema = this.extrema(),
      result = {};
    this.dims.forEach(
      function (d) {
        result[d] = utils.getminmax(this, d, extrema[d]);
      }.bind(this)
    );
    return result;
  }

  overlaps(curve) {
    const lbbox = this.bbox(),
      tbbox = curve.bbox();
    return utils.bboxoverlap(lbbox, tbbox);
  }

  offset(t, d) {
    if (typeof d !== "undefined") {
      const c = this.get(t),
        n = this.normal(t);
      const ret = {
        c: c,
        n: n,
        x: c.x + n.x * d,
        y: c.y + n.y * d,
      };
      if (this._3d) {
        ret.z = c.z + n.z * d;
      }
      return ret;
    }
    if (this._linear) {
      const nv = this.normal(0),
        coords = this.points.map(function (p) {
          const ret = {
            x: p.x + t * nv.x,
            y: p.y + t * nv.y,
          };
          if (p.z && nv.z) {
            ret.z = p.z + t * nv.z;
          }
          return ret;
        });
      return [new Bezier(coords)];
    }
    return this.reduce().map(function (s) {
      if (s._linear) {
        return s.offset(t)[0];
      }
      return s.scale(t);
    });
  }

  simple() {
    if (this.order === 3) {
      const a1 = utils.angle(this.points[0], this.points[3], this.points[1]);
      const a2 = utils.angle(this.points[0], this.points[3], this.points[2]);
      if ((a1 > 0 && a2 < 0) || (a1 < 0 && a2 > 0)) return false;
    }
    const n1 = this.normal(0);
    const n2 = this.normal(1);
    let s = n1.x * n2.x + n1.y * n2.y;
    if (this._3d) {
      s += n1.z * n2.z;
    }
    return abs(acos(s)) < pi / 3;
  }

  reduce() {
    // TODO: examine these var types in more detail...
    let i,
      t1 = 0,
      t2 = 0,
      step = 0.01,
      segment,
      pass1 = [],
      pass2 = [];
    // first pass: split on extrema
    let extrema = this.extrema().values;
    if (extrema.indexOf(0) === -1) {
      extrema = [0].concat(extrema);
    }
    if (extrema.indexOf(1) === -1) {
      extrema.push(1);
    }

    for (t1 = extrema[0], i = 1; i < extrema.length; i++) {
      t2 = extrema[i];
      segment = this.split(t1, t2);
      segment._t1 = t1;
      segment._t2 = t2;
      pass1.push(segment);
      t1 = t2;
    }

    // second pass: further reduce these segments to simple segments
    pass1.forEach(function (p1) {
      t1 = 0;
      t2 = 0;
      while (t2 <= 1) {
        for (t2 = t1 + step; t2 <= 1 + step; t2 += step) {
          segment = p1.split(t1, t2);
          if (!segment.simple()) {
            t2 -= step;
            if (abs(t1 - t2) < step) {
              // we can never form a reduction
              return [];
            }
            segment = p1.split(t1, t2);
            segment._t1 = utils.map(t1, 0, 1, p1._t1, p1._t2);
            segment._t2 = utils.map(t2, 0, 1, p1._t1, p1._t2);
            pass2.push(segment);
            t1 = t2;
            break;
          }
        }
      }
      if (t1 < 1) {
        segment = p1.split(t1, 1);
        segment._t1 = utils.map(t1, 0, 1, p1._t1, p1._t2);
        segment._t2 = p1._t2;
        pass2.push(segment);
      }
    });
    return pass2;
  }

  translate(v, d1, d2) {
    d2 = typeof d2 === "number" ? d2 : d1;

    // TODO: make this take curves with control points outside
    //       of the start-end interval into account

    const o = this.order;
    let d = this.points.map((_, i) => (1 - i / o) * d1 + (i / o) * d2);
    return new Bezier(
      this.points.map((p, i) => ({
        x: p.x + v.x * d[i],
        y: p.y + v.y * d[i],
      }))
    );
  }

  scale(d) {
    const order = this.order;
    let distanceFn = false;
    if (typeof d === "function") {
      distanceFn = d;
    }
    if (distanceFn && order === 2) {
      return this.raise().scale(distanceFn);
    }

    // TODO: add special handling for non-linear degenerate curves.

    const clockwise = this.clockwise;
    const points = this.points;

    if (this._linear) {
      return this.translate(
        this.normal(0),
        distanceFn ? distanceFn(0) : d,
        distanceFn ? distanceFn(1) : d
      );
    }

    const r1 = distanceFn ? distanceFn(0) : d;
    const r2 = distanceFn ? distanceFn(1) : d;
    const v = [this.offset(0, 10), this.offset(1, 10)];
    const np = [];
    const o = utils.lli4(v[0], v[0].c, v[1], v[1].c);

    if (!o) {
      throw new Error("cannot scale this curve. Try reducing it first.");
    }

    // move all points by distance 'd' wrt the origin 'o',
    // and move end points by fixed distance along normal.
    [0, 1].forEach(function (t) {
      const p = (np[t * order] = utils.copy(points[t * order]));
      p.x += (t ? r2 : r1) * v[t].n.x;
      p.y += (t ? r2 : r1) * v[t].n.y;
    });

    if (!distanceFn) {
      // move control points to lie on the intersection of the offset
      // derivative vector, and the origin-through-control vector
      [0, 1].forEach((t) => {
        if (order === 2 && !!t) return;
        const p = np[t * order];
        const d = this.derivative(t);
        const p2 = { x: p.x + d.x, y: p.y + d.y };
        np[t + 1] = utils.lli4(p, p2, o, points[t + 1]);
      });
      return new Bezier(np);
    }

    // move control points by "however much necessary to
    // ensure the correct tangent to endpoint".
    [0, 1].forEach(function (t) {
      if (order === 2 && !!t) return;
      var p = points[t + 1];
      var ov = {
        x: p.x - o.x,
        y: p.y - o.y,
      };
      var rc = distanceFn ? distanceFn((t + 1) / order) : d;
      if (distanceFn && !clockwise) rc = -rc;
      var m = sqrt(ov.x * ov.x + ov.y * ov.y);
      ov.x /= m;
      ov.y /= m;
      np[t + 1] = {
        x: p.x + rc * ov.x,
        y: p.y + rc * ov.y,
      };
    });
    return new Bezier(np);
  }

  outline(d1, d2, d3, d4) {
    d2 = d2 === undefined ? d1 : d2;

    if (this._linear) {
      // TODO: find the actual extrema, because they might
      //       be before the start, or past the end.

      const n = this.normal(0);
      const start = this.points[0];
      const end = this.points[this.points.length - 1];
      let s, mid, e;

      if (d3 === undefined) {
        d3 = d1;
        d4 = d2;
      }

      s = { x: start.x + n.x * d1, y: start.y + n.y * d1 };
      e = { x: end.x + n.x * d3, y: end.y + n.y * d3 };
      mid = { x: (s.x + e.x) / 2, y: (s.y + e.y) / 2 };
      const fline = [s, mid, e];

      s = { x: start.x - n.x * d2, y: start.y - n.y * d2 };
      e = { x: end.x - n.x * d4, y: end.y - n.y * d4 };
      mid = { x: (s.x + e.x) / 2, y: (s.y + e.y) / 2 };
      const bline = [e, mid, s];

      const ls = utils.makeline(bline[2], fline[0]);
      const le = utils.makeline(fline[2], bline[0]);
      const segments = [ls, new Bezier(fline), le, new Bezier(bline)];
      return new PolyBezier(segments);
    }

    const reduced = this.reduce(),
      len = reduced.length,
      fcurves = [];

    let bcurves = [],
      p,
      alen = 0,
      tlen = this.length();

    const graduated = typeof d3 !== "undefined" && typeof d4 !== "undefined";

    function linearDistanceFunction(s, e, tlen, alen, slen) {
      return function (v) {
        const f1 = alen / tlen,
          f2 = (alen + slen) / tlen,
          d = e - s;
        return utils.map(v, 0, 1, s + f1 * d, s + f2 * d);
      };
    }

    // form curve oulines
    reduced.forEach(function (segment) {
      const slen = segment.length();
      if (graduated) {
        fcurves.push(
          segment.scale(linearDistanceFunction(d1, d3, tlen, alen, slen))
        );
        bcurves.push(
          segment.scale(linearDistanceFunction(-d2, -d4, tlen, alen, slen))
        );
      } else {
        fcurves.push(segment.scale(d1));
        bcurves.push(segment.scale(-d2));
      }
      alen += slen;
    });

    // reverse the "return" outline
    bcurves = bcurves
      .map(function (s) {
        p = s.points;
        if (p[3]) {
          s.points = [p[3], p[2], p[1], p[0]];
        } else {
          s.points = [p[2], p[1], p[0]];
        }
        return s;
      })
      .reverse();

    // form the endcaps as lines
    const fs = fcurves[0].points[0],
      fe = fcurves[len - 1].points[fcurves[len - 1].points.length - 1],
      bs = bcurves[len - 1].points[bcurves[len - 1].points.length - 1],
      be = bcurves[0].points[0],
      ls = utils.makeline(bs, fs),
      le = utils.makeline(fe, be),
      segments = [ls].concat(fcurves).concat([le]).concat(bcurves);

    return new PolyBezier(segments);
  }

  outlineshapes(d1, d2, curveIntersectionThreshold) {
    d2 = d2 || d1;
    const outline = this.outline(d1, d2).curves;
    const shapes = [];
    for (let i = 1, len = outline.length; i < len / 2; i++) {
      const shape = utils.makeshape(
        outline[i],
        outline[len - i],
        curveIntersectionThreshold
      );
      shape.startcap.virtual = i > 1;
      shape.endcap.virtual = i < len / 2 - 1;
      shapes.push(shape);
    }
    return shapes;
  }

  intersects(curve, curveIntersectionThreshold) {
    if (!curve) return this.selfintersects(curveIntersectionThreshold);
    if (curve.p1 && curve.p2) {
      return this.lineIntersects(curve);
    }
    if (curve instanceof Bezier) {
      curve = curve.reduce();
    }
    return this.curveintersects(
      this.reduce(),
      curve,
      curveIntersectionThreshold
    );
  }

  lineIntersects(line) {
    const mx = min(line.p1.x, line.p2.x),
      my = min(line.p1.y, line.p2.y),
      MX = max(line.p1.x, line.p2.x),
      MY = max(line.p1.y, line.p2.y);
    return utils.roots(this.points, line).filter((t) => {
      var p = this.get(t);
      return utils.between(p.x, mx, MX) && utils.between(p.y, my, MY);
    });
  }

  selfintersects(curveIntersectionThreshold) {
    // "simple" curves cannot intersect with their direct
    // neighbour, so for each segment X we check whether
    // it intersects [0:x-2][x+2:last].

    const reduced = this.reduce(),
      len = reduced.length - 2,
      results = [];

    for (let i = 0, result, left, right; i < len; i++) {
      left = reduced.slice(i, i + 1);
      right = reduced.slice(i + 2);
      result = this.curveintersects(left, right, curveIntersectionThreshold);
      results.push(...result);
    }
    return results;
  }

  curveintersects(c1, c2, curveIntersectionThreshold) {
    const pairs = [];
    // step 1: pair off any overlapping segments
    c1.forEach(function (l) {
      c2.forEach(function (r) {
        if (l.overlaps(r)) {
          pairs.push({ left: l, right: r });
        }
      });
    });
    // step 2: for each pairing, run through the convergence algorithm.
    let intersections = [];
    pairs.forEach(function (pair) {
      const result = utils.pairiteration(
        pair.left,
        pair.right,
        curveIntersectionThreshold
      );
      if (result.length > 0) {
        intersections = intersections.concat(result);
      }
    });
    return intersections;
  }

  arcs(errorThreshold) {
    errorThreshold = errorThreshold || 0.5;
    return this._iterate(errorThreshold, []);
  }

  _error(pc, np1, s, e) {
    const q = (e - s) / 4,
      c1 = this.get(s + q),
      c2 = this.get(e - q),
      ref = utils.dist(pc, np1),
      d1 = utils.dist(pc, c1),
      d2 = utils.dist(pc, c2);
    return abs(d1 - ref) + abs(d2 - ref);
  }

  _iterate(errorThreshold, circles) {
    let t_s = 0,
      t_e = 1,
      safety;
    // we do a binary search to find the "good `t` closest to no-longer-good"
    do {
      safety = 0;

      // step 1: start with the maximum possible arc
      t_e = 1;

      // points:
      let np1 = this.get(t_s),
        np2,
        np3,
        arc,
        prev_arc;

      // booleans:
      let curr_good = false,
        prev_good = false,
        done;

      // numbers:
      let t_m = t_e,
        prev_e = 1,
        step = 0;

      // step 2: find the best possible arc
      do {
        prev_good = curr_good;
        prev_arc = arc;
        t_m = (t_s + t_e) / 2;
        step++;

        np2 = this.get(t_m);
        np3 = this.get(t_e);

        arc = utils.getccenter(np1, np2, np3);

        //also save the t values
        arc.interval = {
          start: t_s,
          end: t_e,
        };

        let error = this._error(arc, np1, t_s, t_e);
        curr_good = error <= errorThreshold;

        done = prev_good && !curr_good;
        if (!done) prev_e = t_e;

        // this arc is fine: we can move 'e' up to see if we can find a wider arc
        if (curr_good) {
          // if e is already at max, then we're done for this arc.
          if (t_e >= 1) {
            // make sure we cap at t=1
            arc.interval.end = prev_e = 1;
            prev_arc = arc;
            // if we capped the arc segment to t=1 we also need to make sure that
            // the arc's end angle is correct with respect to the bezier end point.
            if (t_e > 1) {
              let d = {
                x: arc.x + arc.r * cos(arc.e),
                y: arc.y + arc.r * sin(arc.e),
              };
              arc.e += utils.angle({ x: arc.x, y: arc.y }, d, this.get(1));
            }
            break;
          }
          // if not, move it up by half the iteration distance
          t_e = t_e + (t_e - t_s) / 2;
        } else {
          // this is a bad arc: we need to move 'e' down to find a good arc
          t_e = t_m;
        }
      } while (!done && safety++ < 100);

      if (safety >= 100) {
        break;
      }

      // console.log("L835: [F] arc found", t_s, prev_e, prev_arc.x, prev_arc.y, prev_arc.s, prev_arc.e);

      prev_arc = prev_arc ? prev_arc : arc;
      circles.push(prev_arc);
      t_s = prev_e;
    } while (t_e < 1);
    return circles;
  }
}

export { Bezier };
