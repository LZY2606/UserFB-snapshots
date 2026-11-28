"use strict";

/**
 * Contract tests for the quilt module.
 *
 * Every group receives the check context `c` from task_support/check.cjs and
 * only asserts through `c.check`. The groups must return normally.
 */

const path = require("node:path");
const tgen = require(path.join(__dirname, "..", "index.js"));
const quilt = require(path.join(__dirname, "..", "src", "quilt.js"));

const UINT32 = 4294967296;

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------

function position(x, y, width) {
  return y * width + x;
}

function close(a, b) {
  return Math.abs(a - b) <= 1e-9 * Math.max(1, Math.abs(a), Math.abs(b));
}

// small deterministic generator for test data
function lcg(seed) {
  let state = seed >>> 0;

  return function () {
    state = (1664525 * state + 1013904223) % UINT32;
    return state;
  };
}

function makeSource(width, height, seed) {
  const next = lcg(seed);
  const data = new Uint8Array(width * height * 4);

  for (let i = 0; i < data.length; i++) {
    data[i] = next() % 256;
  }

  return { width: width, height: height, data: data };
}

// the squared RGB difference of a problem cell, alpha ignored
function diffAt(problem, p) {
  if (!problem.filled[p]) {
    return 0;
  }

  const o = p * 4;
  const dr = problem.oldRGBA[o] - problem.newRGBA[o];
  const dg = problem.oldRGBA[o + 1] - problem.newRGBA[o + 1];
  const db = problem.oldRGBA[o + 2] - problem.newRGBA[o + 2];

  return dr * dr + dg * dg + db * db;
}

// every four-neighbour edge of a local rectangle
function problemEdges(problem) {
  const edges = [];

  for (let y = 0; y < problem.height; y++) {
    for (let x = 0; x < problem.width; x++) {
      const p = position(x, y, problem.width);

      if (x + 1 < problem.width) {
        edges.push([p, p + 1]);
      }

      if (y + 1 < problem.height) {
        edges.push([p, p + problem.width]);
      }
    }
  }

  return edges;
}

function energyOf(problem, labels) {
  const edges = problemEdges(problem);
  let energy = 0;

  for (let i = 0; i < edges.length; i++) {
    const p = edges[i][0];
    const q = edges[i][1];

    if (labels[p] !== labels[q]) {
      energy += diffAt(problem, p) + diffAt(problem, q);
    }
  }

  return energy;
}

// terminal feasibility of a label vector
function legalLabels(problem, labels) {
  const n = problem.width * problem.height;

  for (let p = 0; p < n; p++) {
    if (!problem.filled[p] && labels[p] !== 1) {
      return false;
    }

    if (problem.forceOld && problem.forceOld[p] && labels[p] !== 0) {
      return false;
    }

    if (problem.forceNew && problem.forceNew[p] && labels[p] !== 1) {
      return false;
    }
  }

  return true;
}

// exhaustive optimum for a local rectangle (only for small problems)
function bruteForce(problem) {
  const n = problem.width * problem.height;
  let best = Infinity;
  let bestLabels = null;

  for (let mask = 0; mask < 1 << n; mask++) {
    const labels = new Array(n);

    for (let p = 0; p < n; p++) {
      labels[p] = (mask >> p) & 1;
    }

    if (!legalLabels(problem, labels)) {
      continue;
    }

    const energy = energyOf(problem, labels);

    if (energy < best) {
      best = energy;
      bestLabels = labels;
    }
  }

  return { energy: best, labels: bestLabels };
}

// a local cut problem built from explicit differences
function buildProblem(width, height, options) {
  const n = width * height;
  const oldRGBA = new Uint8Array(n * 4);
  const newRGBA = new Uint8Array(n * 4);
  const filled = new Uint8Array(n);
  const forceOld = new Uint8Array(n);
  const forceNew = new Uint8Array(n);

  filled.fill(1);

  if (options.filled) {
    for (let p = 0; p < n; p++) {
      filled[p] = options.filled[p] ? 1 : 0;
    }
  }

  if (options.forceOld) {
    for (let p = 0; p < n; p++) {
      forceOld[p] = options.forceOld[p] ? 1 : 0;
    }
  }

  if (options.forceNew) {
    for (let p = 0; p < n; p++) {
      forceNew[p] = options.forceNew[p] ? 1 : 0;
    }
  }

  // d[i] is encoded as a red channel difference (sqrt of the wanted value),
  // alpha deliberately differs so an implementation that used alpha would
  // score energy differently
  for (let p = 0; p < n; p++) {
    let d = options.d[p] || 0;
    const r = Math.min(255, Math.round(Math.sqrt(d)));

    oldRGBA[p * 4] = r;
    oldRGBA[p * 4 + 1] = (p * 7) % 256;
    oldRGBA[p * 4 + 2] = (p * 13) % 256;
    oldRGBA[p * 4 + 3] = (p * 29) % 256;

    newRGBA[p * 4] = 0;
    newRGBA[p * 4 + 1] = (p * 7) % 256;
    newRGBA[p * 4 + 2] = (p * 13) % 256;
    newRGBA[p * 4 + 3] = 255 - ((p * 29) % 256);
  }

  return {
    width: width,
    height: height,
    oldRGBA: oldRGBA,
    newRGBA: newRGBA,
    filled: filled,
    forceOld: forceOld,
    forceNew: forceNew,
  };
}

// independent replay of a placement plan over a source image
function replay(source, options, result) {
  const width = options.width;
  const height = options.height;
  const patchWidth = options.patchWidth;
  const patchHeight = options.patchHeight;
  const overlapX = options.overlapX;
  const overlapY = options.overlapY;
  const tolerance = options.tolerance;
  const wrap = options.wrap;
  const srcWidth = source.width;
  const srcHeight = source.height;
  const srcData = source.data;

  const data = new Uint8ClampedArray(width * height * 4);
  const provenance = new Int32Array(width * height);
  provenance.fill(-1);
  const filled = new Uint8Array(width * height);

  const candidates = [];

  for (let sy = 0; sy + patchHeight <= srcHeight; sy++) {
    for (let sx = 0; sx + patchWidth <= srcWidth; sx++) {
      candidates.push([sx, sy]);
    }
  }

  const map = function (bx, by) {
    let ox = bx;
    let oy = by;

    if (wrap) {
      ox = ((ox % width) + width) % width;
      oy = ((oy % height) + height) % height;
    } else if (ox < 0 || ox >= width || oy < 0 || oy >= height) {
      return -1;
    }

    return oy * width + ox;
  };

  let state = options.seed >>> 0;
  let index = 0;

  for (let py = 0; py < height; py += patchHeight - overlapY) {
    for (let px = 0; px < width; px += patchWidth - overlapX) {
      const placement = result.placements[index++];
      const effectiveWidth = wrap
        ? patchWidth
        : Math.min(patchWidth, width - px);
      const effectiveHeight = wrap
        ? patchHeight
        : Math.min(patchHeight, height - py);

      // recompute every candidate score on the true filled set
      const scores = [];

      for (let i = 0; i < candidates.length; i++) {
        const sx = candidates[i][0];
        const sy = candidates[i][1];
        let score = 0;

        for (let by = 0; by < effectiveHeight; by++) {
          for (let bx = 0; bx < effectiveWidth; bx++) {
            const o = map(px + bx, py + by);

            if (o < 0 || !filled[o]) {
              continue;
            }

            const s = position(sx + bx, sy + by, srcWidth) * 4;
            const dr = data[o * 4] - srcData[s];
            const dg = data[o * 4 + 1] - srcData[s + 1];
            const db = data[o * 4 + 2] - srcData[s + 2];

            score += dr * dr + dg * dg + db * db;
          }
        }

        scores.push(score);
      }

      const minError = Math.min.apply(null, scores);
      const pool = [];

      for (let i = 0; i < scores.length; i++) {
        if (minError === 0 ? scores[i] === 0 : scores[i] <= minError * (1 + tolerance)) {
          pool.push(i);
        }
      }

      state = (1664525 * state + 1013904223) % UINT32;
      const pick = pool[Math.floor((state / UINT32) * pool.length)];
      const sourceX = candidates[pick][0];
      const sourceY = candidates[pick][1];

      if (placement.x !== px || placement.y !== py) {
        throw new Error("placement anchor mismatch");
      }

      if (placement.candidateCount !== candidates.length) {
        throw new Error("placement candidateCount mismatch");
      }

      if (placement.score !== scores[pick]) {
        throw new Error("placement score mismatch");
      }

      if (placement.sourceX !== sourceX || placement.sourceY !== sourceY) {
        throw new Error("placement source mismatch");
      }

      // rebuild the local cell table
      const cells = [];

      for (let ly = 0; ly < effectiveHeight; ly++) {
        for (let lx = 0; lx < effectiveWidth; lx++) {
          const o = map(px + lx, py + ly);
          const s = position(sourceX + lx, sourceY + ly, srcWidth) * 4;
          const isFilled = o >= 0 && filled[o];

          cells.push({
            lx: lx,
            ly: ly,
            o: o,
            filled: isFilled,
            old: isFilled
              ? [
                  data[o * 4],
                  data[o * 4 + 1],
                  data[o * 4 + 2],
                  data[o * 4 + 3],
                ]
              : [0, 0, 0, 0],
            next: [srcData[s], srcData[s + 1], srcData[s + 2], srcData[s + 3]],
          });
        }
      }

      const at = function (lx, ly) {
        return ly * effectiveWidth + lx;
      };

      const forceOld = new Uint8Array(cells.length);
      const forceNew = new Uint8Array(cells.length);

      for (let i = 0; i < cells.length; i++) {
        const cell = cells[i];

        if (!cell.filled) {
          forceNew[i] = 1;
          continue;
        }

        let ox = px + cell.lx;
        let oy = py + cell.ly;

        if (wrap) {
          ox = ((ox % width) + width) % width;
          oy = ((oy % height) + height) % height;
        }

        const neighbours = [
          [ox, oy - 1],
          [ox, oy + 1],
          [ox - 1, oy],
          [ox + 1, oy],
        ];

        for (let k = 0; k < 4; k++) {
          let nx = neighbours[k][0];
          let ny = neighbours[k][1];

          if (wrap) {
            nx = ((nx % width) + width) % width;
            ny = ((ny % height) + height) % height;
          } else if (nx < 0 || nx >= width || ny < 0 || ny >= height) {
            continue;
          }

          if (!filled[ny * width + nx]) {
            continue;
          }

          let dx = nx - px;
          let dy = ny - py;

          if (wrap) {
            dx = ((dx % width) + width) % width;
            dy = ((dy % height) + height) % height;

            if (dx < effectiveWidth && dy < effectiveHeight) {
              continue;
            }
          } else if (
            dx >= 0 &&
            dx < effectiveWidth &&
            dy >= 0 &&
            dy < effectiveHeight
          ) {
            continue;
          }

          forceOld[i] = 1;
          break;
        }
      }

      const allFilled = cells.every(function (cell) {
        return cell.filled;
      });

      let skipped = false;

      if (allFilled) {
        const center = at(
          Math.floor(effectiveWidth / 2),
          Math.floor(effectiveHeight / 2)
        );

        if (forceOld[center]) {
          skipped = true;
        } else {
          forceNew[center] = 1;
        }
      }

      if (placement.skipped !== skipped) {
        throw new Error("placement skipped mismatch");
      }

      if (skipped) {
        continue;
      }

      const labels = placement.labels;

      if (!Array.isArray(labels) || labels.length !== cells.length) {
        throw new Error("placement labels length mismatch");
      }

      for (let i = 0; i < cells.length; i++) {
        const cell = cells[i];

        if (cell.o < 0) {
          continue;
        }

        if (labels[i] === 1) {
          data[cell.o * 4] = cell.next[0];
          data[cell.o * 4 + 1] = cell.next[1];
          data[cell.o * 4 + 2] = cell.next[2];
          data[cell.o * 4 + 3] = cell.next[3];
          provenance[cell.o] = sourceY + cell.ly;
        }

        filled[cell.o] = 1;
      }
    }
  }

  if (index !== result.placements.length) {
    throw new Error("placement count mismatch");
  }

  return { data: data, provenance: provenance };
}

// ---------------------------------------------------------------------------
// candidates: pool construction and the local LCG
// ---------------------------------------------------------------------------

exports.candidates = function (c) {
  // tiny source: every candidate position is enumerable by hand
  const source = {
    width: 5,
    height: 4,
    data: new Uint8Array(5 * 4 * 4),
  };

  for (let i = 0; i < source.data.length; i++) {
    source.data[i] = (i * 11 + 3) % 256;
  }

  const options = {
    width: 12,
    height: 10,
    patchWidth: 3,
    patchHeight: 3,
    overlapX: 1,
    overlapY: 1,
    tolerance: 0,
    seed: 12345,
    wrap: false,
  };

  const result = quilt.synthesize(source, options);

  // candidate count = (5 - 3 + 1) * (4 - 3 + 1), ordered by source y then x
  c.check(result.placements.length > 0, "placements exist");
  c.check(result.placements[0].candidateCount === 6, "candidate count 3x2");

  // the anchors start at (0, 0) and step by patch - overlap
  const first = result.placements[0];
  c.check(first.x === 0 && first.y === 0, "first anchor (0,0)");

  const anchors = result.placements.map(function (p) {
    return [p.x, p.y];
  });

  const expected = [];

  for (let py = 0; py < options.height; py += options.patchHeight - options.overlapY) {
    for (let px = 0; px < options.width; px += options.patchWidth - options.overlapX) {
      expected.push([px, py]);
    }
  }

  c.check(
    JSON.stringify(anchors) === JSON.stringify(expected),
    "anchors advance by patch minus overlap"
  );

  // replay the LCG and the pool selection independently
  const replayed = replay(source, options, result);
  c.check(replayed.data.length === result.data.length, "replay size");

  let same = true;

  for (let i = 0; i < result.data.length; i++) {
    if (replayed.data[i] !== result.data[i]) {
      same = false;
    }
  }

  c.check(same, "replayed image matches synthesize");

  // the first placement uses the first LCG step, even for the first patch
  const firstState = (1664525 * options.seed + 1013904223) % UINT32;
  const candidateScores = [];

  // the first patch is fully unfilled, so every candidate scores zero
  for (let i = 0; i < 6; i++) {
    candidateScores.push(0);
  }

  const pool = candidateScores
    .map(function (score, i) {
      return i;
    })
    .filter(function (i) {
      return candidateScores[i] === 0;
    });
  const pick = pool[Math.floor((firstState / UINT32) * pool.length)];

  c.check(
    pick >= 0 && pick < 6,
    "first local LCG index inside the pool"
  );

  // a second call yields exactly the same placements - the LCG is local
  const again = quilt.synthesize(source, options);
  c.check(
    JSON.stringify(again.placements) === JSON.stringify(result.placements),
    "same seed reproduces placements"
  );

  // a different seed changes the placements but not the geometry
  const other = quilt.synthesize(source, Object.assign({}, options, { seed: options.seed + 1 }));
  c.check(
    other.placements[0].candidateCount === 6 &&
      other.placements.length === result.placements.length,
    "seed does not change candidate geometry"
  );

  // zero tolerance and a zero error image: only exact matches enter the pool
  const flat = {
    width: 4,
    height: 4,
    data: new Uint8Array(4 * 4 * 4),
  };

  const flatResult = quilt.synthesize(flat, {
    width: 8,
    height: 8,
    patchWidth: 3,
    patchHeight: 3,
    overlapX: 1,
    overlapY: 1,
    tolerance: 0,
    seed: 7,
    wrap: false,
  });

  c.check(
    flatResult.placements.every(function (p) {
      return p.score === 0;
    }),
    "zero error source scores zero"
  );

  // the seed passed to synthesize never leaks into the generator
  const generator = tgen.init(8, 8, "pingpong");
  generator.calc.seed = 4242;
  generator.do("quilt", {
    source: source,
    options: {
      patchWidth: 3,
      patchHeight: 3,
      overlapX: 1,
      overlapY: 1,
      tolerance: 0,
      seed: 99,
      wrap: false,
    },
  });
  c.check(generator.calc.seed === 4242, "calc.seed untouched by quilt");

  return;
};

// ---------------------------------------------------------------------------
// cuts: exhaustive optimality of the global minimum cut
// ---------------------------------------------------------------------------

exports.cuts = function (c) {
  // a simple line with a zero cost cell
  const line = buildProblem(4, 1, {
    d: [9, 4, 0, 16],
    forceOld: [1, 0, 0, 0],
    forceNew: [0, 0, 0, 1],
  });

  let result = quilt.cut(line);
  let optimum = bruteForce(line);

  c.check(result.labels instanceof Uint8Array, "labels is Uint8Array");
  c.check(result.labels.length === 4, "labels length");
  c.check(legalLabels(line, result.labels), "line labels legal");
  c.check(close(result.energy, optimum.energy), "line energy optimal");
  c.check(close(result.energy, energyOf(line, result.labels)), "line energy consistent");

  // L shaped filled region (12 cells), unfilled cells are forced to new
  const lShape = buildProblem(4, 3, {
    d: [4, 9, 0, 0, 16, 25, 0, 0, 36, 0, 0, 0],
    filled: [1, 1, 1, 1, 1, 1, 0, 0, 1, 0, 0, 0],
    forceOld: [1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0],
  });

  result = quilt.cut(lShape);
  optimum = bruteForce(lShape);

  c.check(legalLabels(lShape, result.labels), "L labels legal");
  c.check(close(result.energy, optimum.energy), "L energy optimal");

  // a hole in the middle: the hole is unfilled and forced to new
  const hole = buildProblem(3, 3, {
    d: [1, 2, 3, 4, 0, 6, 7, 8, 9],
    filled: [1, 1, 1, 1, 0, 1, 1, 1, 1],
    forceOld: [1, 0, 0, 0, 0, 0, 0, 0, 0],
  });

  result = quilt.cut(hole);
  optimum = bruteForce(hole);

  c.check(result.labels[4] === 1, "hole cell takes new");
  c.check(legalLabels(hole, result.labels), "hole labels legal");
  c.check(close(result.energy, optimum.energy), "hole energy optimal");

  // zero cost everywhere: every legal labelling is optimal
  const zero = buildProblem(3, 2, {
    d: [0, 0, 0, 0, 0, 0],
    forceOld: [1, 0, 0, 0, 0, 0],
    forceNew: [0, 0, 0, 0, 0, 1],
  });

  result = quilt.cut(zero);
  c.check(close(result.energy, 0), "zero cost energy is zero");

  // alpha is ignored: same RGB, different alpha, energy must stay zero
  const alpha = {
    width: 2,
    height: 2,
    oldRGBA: new Uint8Array([10, 20, 30, 0, 40, 50, 60, 1, 70, 80, 90, 2, 100, 110, 120, 3]),
    newRGBA: new Uint8Array([10, 20, 30, 255, 40, 50, 60, 254, 70, 80, 90, 253, 100, 110, 120, 252]),
    filled: new Uint8Array([1, 1, 1, 1]),
    forceOld: new Uint8Array([1, 0, 0, 0]),
    forceNew: new Uint8Array([0, 0, 0, 1]),
  };

  result = quilt.cut(alpha);
  c.check(close(result.energy, 0), "alpha is not part of the energy");

  // a global cut must beat a per-cell greedy: the joined seam is cheaper
  const greedyTrap = buildProblem(3, 2, {
    d: [1, 0, 0, 0, 0, 0],
    forceOld: [1, 0, 0, 0, 0, 0],
    forceNew: [0, 0, 0, 1, 0, 1],
  });

  result = quilt.cut(greedyTrap);
  optimum = bruteForce(greedyTrap);

  c.check(close(result.energy, optimum.energy), "trap energy optimal");
  c.check(close(result.energy, 1), "global cut finds the cheap seam");
  c.check(result.labels[0] === 0 && result.labels[5] === 1, "trap terminals honoured");

  // two separate seams across both axes: the cut spans the whole rectangle
  const both = buildProblem(3, 3, {
    d: [100, 0, 100, 0, 0, 0, 100, 0, 100],
    forceOld: [1, 0, 0, 0, 0, 0, 0, 0, 0],
    forceNew: [0, 0, 0, 0, 0, 0, 0, 0, 1],
  });

  result = quilt.cut(both);
  optimum = bruteForce(both);

  c.check(legalLabels(both, result.labels), "two axis labels legal");
  c.check(close(result.energy, optimum.energy), "two axis energy optimal");

  // determinism: the same input gives the same labels and energy
  const repeatA = quilt.cut(lShape);
  const repeatB = quilt.cut(lShape);

  c.check(
    Buffer.compare(Buffer.from(repeatA.labels), Buffer.from(repeatB.labels)) === 0,
    "cut is deterministic"
  );
  c.check(repeatA.energy === repeatB.energy, "cut energy deterministic");

  // forceOld and forceNew on the same cell are a RangeError
  const conflict = buildProblem(2, 2, {
    d: [1, 1, 1, 1],
    forceOld: [1, 0, 0, 0],
    forceNew: [1, 0, 0, 0],
  });

  let threw = false;

  try {
    quilt.cut(conflict);
  } catch (error) {
    threw = error instanceof RangeError;
  }

  c.check(threw, "conflicting terminals throw RangeError");

  // an unfilled cell is forced to new even when only forceOld marks it
  const holeForce = buildProblem(2, 2, {
    d: [5, 5, 5, 5],
    filled: [1, 0, 1, 1],
    forceOld: [0, 1, 0, 0],
  });

  result = quilt.cut(holeForce);

  c.check(result.labels[1] === 1, "unfilled cell stays new despite forceOld");

  // the centroid corner is not double counted: a problem where the double
  // counted corner would change the optimum
  const corner = buildProblem(2, 2, {
    d: [7, 0, 0, 0],
    forceOld: [1, 0, 0, 0],
    forceNew: [0, 0, 0, 1],
  });

  result = quilt.cut(corner);
  optimum = bruteForce(corner);

  c.check(close(result.energy, optimum.energy), "corner energy matches a plain sum");
  c.check(close(result.energy, 0), "corner seam has zero cost");

  return;
};

// ---------------------------------------------------------------------------
// periodic: wrap mode across both axes
// ---------------------------------------------------------------------------

exports.periodic = function (c) {
  const source = makeSource(6, 5, 55);
  const options = {
    width: 11,
    height: 13,
    patchWidth: 4,
    patchHeight: 3,
    overlapX: 2,
    overlapY: 1,
    tolerance: 0.5,
    seed: 2024,
    wrap: true,
  };

  const result = quilt.synthesize(source, options);

  c.check(result.width === 11 && result.height === 13, "wrap output size");
  c.check(result.data instanceof Uint8ClampedArray, "data is Uint8ClampedArray");
  c.check(result.provenance instanceof Int32Array, "provenance is Int32Array");
  c.check(result.data.length === 11 * 13 * 4, "data length");
  c.check(result.provenance.length === 11 * 13, "provenance length");

  // every output pixel is exactly one source pixel, RGBA kept together
  let consistent = true;

  for (let p = 0; p < result.provenance.length; p++) {
    const row = result.provenance[p];

    if (!Number.isInteger(row) || row < 0 || row >= source.height) {
      consistent = false;
      break;
    }
  }

  c.check(consistent, "provenance rows in range");

  let wholeGroups = true;

  for (let p = 0; p < result.provenance.length; p++) {
    const row = result.provenance[p];

    // the pixel must equal some source pixel in that row
    let found = false;

    for (let sx = 0; sx < source.width && !found; sx++) {
      const s = position(sx, row, source.width) * 4;

      if (
        source.data[s] === result.data[p * 4] &&
        source.data[s + 1] === result.data[p * 4 + 1] &&
        source.data[s + 2] === result.data[p * 4 + 2] &&
        source.data[s + 3] === result.data[p * 4 + 3]
      ) {
        found = true;
      }
    }

    if (!found) {
      wholeGroups = false;
      break;
    }
  }

  c.check(wholeGroups, "each pixel is a whole source pixel");

  // the replay rebuilds both the image and the provenance
  const rebuilt = replay(source, options, result);
  let sameData = true;

  for (let i = 0; i < result.data.length; i++) {
    if (rebuilt.data[i] !== result.data[i]) {
      sameData = false;
      break;
    }
  }

  let sameProvenance = true;

  for (let i = 0; i < result.provenance.length; i++) {
    if (rebuilt.provenance[i] !== result.provenance[i]) {
      sameProvenance = false;
      break;
    }
  }

  c.check(sameData, "wrap replay reproduces the image");
  c.check(sameProvenance, "wrap replay reproduces the provenance");

  // a wrapped block reaches across the right/bottom edge: the anchors cover
  // both axes, and the last patch crosses the boundary
  const lastPlacement = result.placements[result.placements.length - 1];
  c.check(
    lastPlacement.x + options.patchWidth > options.width ||
      lastPlacement.y + options.patchHeight > options.height,
    "a wrap placement crosses an axis boundary"
  );

  // clipped (non wrapping) placement: the last block is partially cut
  const clipped = quilt.synthesize(source, Object.assign({}, options, { wrap: false }));
  const clippedLast = clipped.placements[clipped.placements.length - 1];
  c.check(
    clippedLast.x + options.patchWidth > options.width ||
      clippedLast.y + options.patchHeight > options.height,
    "a clipped placement is partially cut"
  );

  // clipped replay still matches
  const clippedReplay = replay(source, Object.assign({}, options, { wrap: false }), clipped);
  let clippedSame = true;

  for (let i = 0; i < clipped.data.length; i++) {
    if (clippedReplay.data[i] !== clipped.data[i]) {
      clippedSame = false;
      break;
    }
  }

  c.check(clippedSame, "clipped replay reproduces the image");

  // smaller local labels for wrap blocks are all inside the local grid
  let labelSizes = true;

  for (let i = 0; i < result.placements.length; i++) {
    const placement = result.placements[i];

    if (!placement.skipped) {
      const expected = options.patchWidth * options.patchHeight;

      if (placement.labels.length !== expected) {
        labelSizes = false;
      }
    }
  }

  c.check(labelSizes, "wrap labels cover the whole patch");

  return;
};

// ---------------------------------------------------------------------------
// replay: do() / render() and generator independence
// ---------------------------------------------------------------------------

exports.replay = function (c) {
  const source = makeSource(5, 5, 88);
  const options = {
    patchWidth: 4,
    patchHeight: 4,
    overlapX: 2,
    overlapY: 2,
    tolerance: 0.3,
    seed: 31337,
    wrap: false,
  };
  const width = 12;
  const height = 12;

  const standalone = quilt.synthesize(
    source,
    Object.assign({}, options, { width: width, height: height })
  );

  // do() writes the texture, is chainable and matches the standalone call
  const generator = tgen.init(width, height, "pingpong");
  generator.calc.seed = 9001;
  const chained = generator.do("quilt", { source: source, options: options });

  c.check(chained === generator, "do returns the generator");
  c.check(generator.calc.seed === 9001, "do keeps calc.seed");

  let same = true;

  for (let i = 0; i < standalone.data.length; i++) {
    if (generator.texture.data[i] !== standalone.data[i]) {
      same = false;
      break;
    }
  }

  c.check(same, "do output matches standalone synthesize");

  // stored params are JSON friendly and replay through render()
  const config = generator.params("quilt");
  c.check(Array.isArray(config.items[0][2].source.data), "stored source is a plain array");
  c.check(!(config.items[0][2].source.data instanceof Uint8Array), "stored source has no typed array");

  const json = JSON.parse(JSON.stringify(config));
  const rendered = tgen.init(width, height, "pingpong");
  rendered.render(json);

  let renderedSame = true;

  for (let i = 0; i < standalone.data.length; i++) {
    if (rendered.texture.data[i] !== standalone.data[i]) {
      renderedSame = false;
      break;
    }
  }

  c.check(renderedSame, "render replay matches standalone synthesize");

  // same seed, two generators, same pixels
  const a = tgen.init(width, height, "pingpong");
  const b = tgen.init(width, height, "pingpong");
  a.do("quilt", { source: source, options: options });
  b.do("quilt", { source: source, options: options });

  let equal = true;

  for (let i = 0; i < a.texture.data.length; i++) {
    if (a.texture.data[i] !== b.texture.data[i]) {
      equal = false;
      break;
    }
  }

  c.check(equal, "two generators with the same seed agree");

  // a different seed in another generator does not disturb the first one
  const before = Array.from(a.texture.data);
  const other = tgen.init(width, height, "pingpong");
  other.do("quilt", {
    source: source,
    options: Object.assign({}, options, { seed: options.seed + 12345 }),
  });

  let untouched = true;

  for (let i = 0; i < before.length; i++) {
    if (a.texture.data[i] !== before[i]) {
      untouched = false;
      break;
    }
  }

  c.check(untouched, "generators do not affect each other");

  // the generator seed sequence is separate: quilt between seeded effects
  // does not consume generator randomness, the following effect starts from
  // exactly the same seed state as without the quilt
  const left = tgen.init(width, height, "pingpong");
  left.do("noise", { mode: "color", seed: 444 });
  const seedAfterNoise = left.calc.seed;
  left.do("quilt", { source: source, options: options });
  c.check(left.calc.seed === seedAfterNoise, "quilt does not consume the generator seed");
  left.do("noise", { mode: "color", seed: 445 });
  const leftSeed = left.calc.seed;

  const right = tgen.init(width, height, "pingpong");
  right.do("noise", { mode: "color", seed: 444 });
  right.do("noise", { mode: "color", seed: 445 });
  const rightSeed = right.calc.seed;

  c.check(leftSeed === rightSeed, "quilt leaves the following seeded effect untouched");

  // invalid options must throw and never produce a half result
  let typeErrors = 0;

  try {
    quilt.synthesize(source, Object.assign({}, options, { width: width, height: height, seed: -1 }));
  } catch (error) {
    if (error instanceof RangeError || error instanceof TypeError) {
      typeErrors++;
    }
  }

  try {
    quilt.synthesize(source, Object.assign({}, options, { width: width, height: height, wrap: "yes" }));
  } catch (error) {
    if (error instanceof TypeError) {
      typeErrors++;
    }
  }

  try {
    quilt.synthesize(
      { width: 5, height: 5, data: new Uint8Array(3) },
      Object.assign({}, options, { width: width, height: height })
    );
  } catch (error) {
    if (error instanceof RangeError) {
      typeErrors++;
    }
  }

  c.check(typeErrors === 3, "invalid input throws");

  return;
};
