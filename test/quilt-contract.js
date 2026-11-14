"use strict";

/**
 * Contract tests for src/quilt.js.
 *
 * Every group receives the check context from task_support/check.cjs and
 * only uses c.check(condition, message); nothing is printed on success.
 * Node built-ins and CommonJS only.
 */

const fs = require("node:fs");
const path = require("node:path");
const quilt = require("../src/quilt.js");
const tgen = require("../index.js");

const LCG_MULTIPLIER = 1664525;
const LCG_INCREMENT = 1013904223;
const LCG_MODULUS = 4294967296;

const lcgStep = function (state) {
  return (LCG_MULTIPLIER * state + LCG_INCREMENT) % LCG_MODULUS;
};

const makeSource = function (seed, width, height, alphaPattern) {
  const data = new Uint8ClampedArray(width * height * 4);
  let state = seed >>> 0;

  for (let i = 0; i < width * height; i++) {
    for (let k = 0; k < 4; k++) {
      state = lcgStep(state);
      data[i * 4 + k] = Math.floor((state / LCG_MODULUS) * 256);
    }

    data[i * 4 + 3] = alphaPattern
      ? alphaPattern(i)
      : i % 5 === 0
        ? 128
        : 255;
  }

  return { width, height, data };
};

const rgbSquaredError = function (a, offsetA, b, offsetB) {
  const dr = a[offsetA] - b[offsetB];
  const dg = a[offsetA + 1] - b[offsetB + 1];
  const db = a[offsetA + 2] - b[offsetB + 2];

  return dr * dr + dg * dg + db * db;
};

// exhaustive optimum for grids of at most 12 cells
const bruteForceCut = function (problem) {
  const { width, height, oldRGBA, newRGBA, filled, forceOld, forceNew } =
    problem;
  const cells = width * height;
  const d = new Float64Array(cells);

  for (let p = 0; p < cells; p++) {
    if (!filled[p]) {
      d[p] = 0;
      continue;
    }

    d[p] = rgbSquaredError(oldRGBA, p * 4, newRGBA, p * 4);
  }

  const labelEnergy = function (labels) {
    let energy = 0;

    for (let y = 0; y < height; y++) {
      for (let x = 0; x < width; x++) {
        const p = y * width + x;

        if (x + 1 < width && labels[p] !== labels[p + 1]) {
          energy += d[p] + d[p + 1];
        }

        if (
          y + 1 < height &&
          labels[p] !== labels[(y + 1) * width + x]
        ) {
          energy += d[p] + d[p + width];
        }
      }
    }

    return energy;
  };

  const legal = function (labels) {
    for (let p = 0; p < cells; p++) {
      if (!filled[p] && labels[p] !== 1) return false;
      if (forceOld && forceOld[p] && labels[p] !== 0) return false;
      if (forceNew && forceNew[p] && labels[p] !== 1) return false;
    }

    return true;
  };

  let bestEnergy = Infinity;
  let bestLabels = null;

  for (let mask = 0; mask < 1 << cells; mask++) {
    const labels = new Uint8Array(cells);

    for (let p = 0; p < cells; p++) {
      labels[p] = (mask >> p) & 1;
    }

    if (!legal(labels)) {
      continue;
    }

    const energy = labelEnergy(labels);

    if (energy < bestEnergy) {
      bestEnergy = energy;
      bestLabels = labels;
    }
  }

  return { bestEnergy, bestLabels, d, labelEnergy };
};

// row-major per-cell greedy: the joint-cut degeneration that must lose
const greedyCut = function (width, height, d, filled, forceOld, forceNew) {
  const cells = width * height;
  const labels = new Uint8Array(cells).fill(2);

  for (let p = 0; p < cells; p++) {
    if (forceOld && forceOld[p]) {
      labels[p] = 0;
    } else if ((forceNew && forceNew[p]) || !filled[p]) {
      labels[p] = 1;
    }
  }

  for (let p = 0; p < cells; p++) {
    if (labels[p] !== 2) {
      continue;
    }

    const x = p % width;
    const y = (p / width) | 0;
    const neighbours = [];

    if (x > 0) neighbours.push(p - 1);
    if (x + 1 < width) neighbours.push(p + 1);
    if (y > 0) neighbours.push(p - width);
    if (y + 1 < height) neighbours.push(p + width);

    let cost0 = 0;
    let cost1 = 0;

    for (const q of neighbours) {
      if (labels[q] === 2) {
        continue;
      }

      if (labels[q] !== 0) cost0 += d[p] + d[q];
      if (labels[q] !== 1) cost1 += d[p] + d[q];
    }

    labels[p] = cost1 < cost0 ? 1 : 0;
  }

  let energy = 0;

  for (let y = 0; y < height; y++) {
    for (let x = 0; x < width; x++) {
      const p = y * width + x;

      if (x + 1 < width && labels[p] !== labels[p + 1]) {
        energy += d[p] + d[p + 1];
      }

      if (y + 1 < height && labels[p] !== labels[p + width]) {
        energy += d[p] + d[p + width];
      }
    }
  }

  return { labels, energy };
};

// rebuild the output state up to (but not including) one placement from the
// recorded labels, then enumerate every source candidate independently
const enumerateCandidates = function (result, source, options) {
  const filled = new Uint8Array(result.width * result.height);
  const output = new Uint8ClampedArray(result.width * result.height * 4);
  let lcgState = options.seed >>> 0;
  const reports = [];

  for (const placement of result.placements) {
    const localWidth = options.wrap
      ? options.patchWidth
      : Math.min(options.patchWidth, result.width - placement.x);
    const localHeight = options.wrap
      ? options.patchHeight
      : Math.min(options.patchHeight, result.height - placement.y);

    const mapX = function (lx) {
      return options.wrap
        ? (placement.x + lx) % result.width
        : placement.x + lx;
    };

    const mapY = function (ly) {
      return options.wrap
        ? (placement.y + ly) % result.height
        : placement.y + ly;
    };

    let anyFilled = false;
    const localFilled = new Uint8Array(localWidth * localHeight);

    for (let ly = 0; ly < localHeight; ly++) {
      for (let lx = 0; lx < localWidth; lx++) {
        const cell = ly * localWidth + lx;
        const outCell = mapY(ly) * result.width + mapX(lx);
        localFilled[cell] = filled[outCell] ? 1 : 0;
        anyFilled = anyFilled || filled[outCell] === 1;
      }
    }

    const maxSourceX = source.width - options.patchWidth;
    const maxSourceY = source.height - options.patchHeight;
    const scored = [];
    let minError = Infinity;

    for (let sourceY = 0; sourceY <= maxSourceY; sourceY++) {
      for (let sourceX = 0; sourceX <= maxSourceX; sourceX++) {
        let error = 0;

        if (anyFilled) {
          for (let ly = 0; ly < localHeight; ly++) {
            for (let lx = 0; lx < localWidth; lx++) {
              const cell = ly * localWidth + lx;

              if (!localFilled[cell]) {
                continue;
              }

              const outOffset =
                (mapY(ly) * result.width + mapX(lx)) * 4;
              const sourceOffset =
                ((sourceY + ly) * source.width + (sourceX + lx)) * 4;

              // every filled cell contributes exactly once
              error += rgbSquaredError(
                source.data,
                sourceOffset,
                output,
                outOffset
              );
            }
          }
        }

        scored.push({ sourceX, sourceY, error });

        if (error < minError) {
          minError = error;
        }
      }
    }

    // the same buggy bookkeeping a corner-double-count implementation would
    // produce: cells lying in both overlap strips are scored twice
    const cornerScored = scored.map(function (entry) {
      let error = 0;

      for (let ly = 0; ly < localHeight; ly++) {
        for (let lx = 0; lx < localWidth; lx++) {
          const cell = ly * localWidth + lx;

          if (!localFilled[cell]) {
            continue;
          }

          const outOffset =
            (mapY(ly) * result.width + mapX(lx)) * 4;
          const sourceOffset =
            ((entry.sourceY + ly) * source.width +
              (entry.sourceX + lx)) *
            4;
          const cellError = rgbSquaredError(
            source.data,
            sourceOffset,
            output,
            outOffset
          );
          const corner =
            lx < options.overlapX && ly < options.overlapY;

          error += corner ? cellError * 2 : cellError;
        }
      }

      return { ...entry, cornerError: error };
    });

    let cornerArgMin = cornerScored[0];

    for (const entry of cornerScored) {
      if (entry.cornerError < cornerArgMin.cornerError) {
        cornerArgMin = entry;
      }
    }

    const pool = [];

    if (minError === 0) {
      for (const entry of scored) {
        if (entry.error === 0) pool.push(entry);
      }
    } else {
      const threshold = minError * (1 + options.tolerance);

      for (const entry of scored) {
        if (entry.error <= threshold) pool.push(entry);
      }
    }

    lcgState = lcgStep(lcgState);
    const poolIndex = Math.floor((lcgState / LCG_MODULUS) * pool.length);
    const picked = pool[poolIndex];

    reports.push({
      placement,
      scored,
      pool,
      minError,
      picked,
      cornerArgMin,
      localWidth,
      localHeight,
      allFilled: localFilled.every((value) => value === 1),
    });

    // replay the recorded labels onto the independent state
    if (!placement.skipped) {
      for (let ly = 0; ly < localHeight; ly++) {
        for (let lx = 0; lx < localWidth; lx++) {
          const cell = ly * localWidth + lx;

          if (placement.labels[cell] !== 1) {
            continue;
          }

          const outCell = mapY(ly) * result.width + mapX(lx);
          const sourceOffset =
            ((placement.sourceY + ly) * source.width +
              (placement.sourceX + lx)) *
            4;

          for (let k = 0; k < 4; k++) {
            output[outCell * 4 + k] = source.data[sourceOffset + k];
          }

          filled[outCell] = 1;
        }
      }
    }
  }

  return reports;
};

// replay the final image and provenance from placements alone
const replayPlacements = function (result, source, options) {
  const data = new Uint8ClampedArray(result.width * result.height * 4);
  const provenance = new Int32Array(result.width * result.height).fill(-1);

  for (const placement of result.placements) {
    if (placement.skipped) {
      continue;
    }

    const localWidth = options.wrap
      ? options.patchWidth
      : Math.min(options.patchWidth, result.width - placement.x);
    const localHeight = options.wrap
      ? options.patchHeight
      : Math.min(options.patchHeight, result.height - placement.y);

    for (let ly = 0; ly < localHeight; ly++) {
      for (let lx = 0; lx < localWidth; lx++) {
        const cell = ly * localWidth + lx;

        if (placement.labels[cell] !== 1) {
          continue;
        }

        const outX = options.wrap
          ? (placement.x + lx) % result.width
          : placement.x + lx;
        const outY = options.wrap
          ? (placement.y + ly) % result.height
          : placement.y + ly;
        const outCell = outY * result.width + outX;
        const sourceX = placement.sourceX + lx;
        const sourceY = placement.sourceY + ly;
        const sourceOffset = (sourceY * source.width + sourceX) * 4;

        for (let k = 0; k < 4; k++) {
          data[outCell * 4 + k] = source.data[sourceOffset + k];
        }

        provenance[outCell] = sourceY * source.width + sourceX;
      }
    }
  }

  return { data, provenance };
};

module.exports = {
  // candidate pool, scoring rules and the local LCG
  candidates(c) {
    const options = {
      width: 8,
      height: 6,
      patchWidth: 4,
      patchHeight: 3,
      overlapX: 2,
      overlapY: 1,
      tolerance: 0.25,
      seed: 1234567,
      wrap: false,
    };

    // scan small deterministic sources until one makes the corner
    // double-counting bookkeeping select a different candidate
    let chosen = null;

    for (let sourceSeed = 1; sourceSeed <= 40 && chosen === null; sourceSeed++) {
      const candidateSource = makeSource(sourceSeed, 6, 4);
      const candidateResult = quilt.synthesize(candidateSource, options);
      const candidateReports = enumerateCandidates(
        candidateResult,
        candidateSource,
        options
      );
      const divergent = candidateReports.find(
        (report) =>
          report.picked.sourceX !== report.cornerArgMin.sourceX ||
          report.picked.sourceY !== report.cornerArgMin.sourceY
      );

      if (divergent) {
        chosen = {
          source: candidateSource,
          result: candidateResult,
          reports: candidateReports,
        };
      }
    }

    c.check(chosen !== null, "found a deterministic corner-divergence case");

    const source = chosen.source;
    const result = chosen.result;
    const reports = chosen.reports;

    c.check(
      result.placements.length === reports.length &&
        result.placements.length > 0,
      "one report per placement"
    );

    let sawOverlapPlacement = false;
    let cornerDivergenceFound = false;

    reports.forEach(function (report, index) {
      const placement = report.placement;

      c.check(
        placement.candidateCount === report.pool.length,
        `placement ${index}: candidate pool size`
      );
      c.check(
        placement.sourceX === report.picked.sourceX &&
          placement.sourceY === report.picked.sourceY,
        `placement ${index}: LCG pool pick`
      );
      c.check(
        placement.score === report.picked.error,
        `placement ${index}: recorded score`
      );

      if (report.minError === 0) {
        c.check(
          report.pool.every((entry) => entry.error === 0),
          `placement ${index}: zero min error admits zero-error candidates only`
        );
      } else {
        c.check(
          report.pool.every(
            (entry) =>
              entry.error <= report.minError * (1 + options.tolerance)
          ),
          `placement ${index}: tolerance threshold`
        );
      }

      if (report.scored.some((entry) => entry.error > 0)) {
        sawOverlapPlacement = true;
      }

      if (
        report.cornerArgMin.sourceX !== report.picked.sourceX ||
        report.cornerArgMin.sourceY !== report.picked.sourceY
      ) {
        cornerDivergenceFound = true;
      }
    });

    c.check(sawOverlapPlacement, "synthesis reaches overlap placements");
    c.check(
      cornerDivergenceFound,
      "double counting an overlap corner changes the chosen candidate"
    );

    // the first placement still consumes exactly one LCG update and its
    // pool contains every source position because no pixel is filled yet
    const first = reports[0];
    const expectedState = lcgStep(options.seed >>> 0);
    const expectedIndex = Math.floor(
      (expectedState / LCG_MODULUS) * first.pool.length
    );

    c.check(
      first.pool.length ===
        (source.width - options.patchWidth + 1) *
          (source.height - options.patchHeight + 1),
      "first placement pools every fitting source position"
    );
    c.check(
      first.placement.sourceX === first.pool[expectedIndex].sourceX &&
        first.placement.sourceY === first.pool[expectedIndex].sourceY,
      "first placement follows the same single LCG update rule"
    );

    // a structured source with repeated columns creates zero-error matches
    const repeatedWidth = 6;
    const repeatedHeight = 4;
    const repeated = new Uint8ClampedArray(
      repeatedWidth * repeatedHeight * 4
    );

    for (let y = 0; y < repeatedHeight; y++) {
      for (let x = 0; x < repeatedWidth; x++) {
        const offset = (y * repeatedWidth + x) * 4;
        const value = ((x % 2) * 200 + y * 17) & 255;
        repeated[offset] = value;
        repeated[offset + 1] = (value + 40) & 255;
        repeated[offset + 2] = (255 - value) & 255;
        repeated[offset + 3] = 255;
      }
    }

    const repeatedSource = {
      width: repeatedWidth,
      height: repeatedHeight,
      data: repeated,
    };
    const repeatedResult = quilt.synthesize(repeatedSource, {
      width: 8,
      height: 6,
      patchWidth: 4,
      patchHeight: 3,
      overlapX: 2,
      overlapY: 1,
      tolerance: 1,
      seed: 99,
      wrap: false,
    });
    const repeatedReports = enumerateCandidates(
      repeatedResult,
      repeatedSource,
      {
        width: 8,
        height: 6,
        patchWidth: 4,
        patchHeight: 3,
        overlapX: 2,
        overlapY: 1,
        tolerance: 1,
        seed: 99,
        wrap: false,
      }
    );
    const zeroReport = repeatedReports.find(
      (report) =>
        report.minError === 0 &&
        report.pool.length < report.scored.length
    );

    c.check(
      zeroReport !== undefined,
      "a zero-error overlap placement coexists with worse candidates"
    );
    c.check(
      zeroReport.pool.every((entry) => entry.error === 0) &&
        zeroReport.scored.some((entry) => entry.error > 0),
      "min error zero keeps only zero-error candidates"
    );

    // plain array source data and TypedArray source data behave identically
    const arraySource = {
      width: source.width,
      height: source.height,
      data: Array.from(source.data),
    };
    const fromArray = quilt.synthesize(arraySource, options);
    const fromTyped = quilt.synthesize(source, options);

    c.check(
      Array.from(fromArray.data).every((v, i) => v === fromTyped.data[i]),
      "array and TypedArray sources synthesize the same texture"
    );
    c.check(
      arraySource.data.every((v, i) => v === source.data[i]),
      "the source input is not modified"
    );
  },

  // global minimum cut against full enumeration on small grids
  cuts(c) {
    const rgbaFromD = function (d, cells) {
      const oldRGBA = new Uint8ClampedArray(cells * 4);
      const newRGBA = new Uint8ClampedArray(cells * 4);

      for (let p = 0; p < cells; p++) {
        oldRGBA[p * 4] = Math.round(Math.sqrt(d[p]));
        oldRGBA[p * 4 + 1] = Math.round(Math.sqrt(d[p]) / 2);
        oldRGBA[p * 4 + 3] = 255;
        // alpha deliberately differs but must never enter the energy
        newRGBA[p * 4 + 3] = 7;
      }

      return { oldRGBA, newRGBA };
    };

    const assertOptimal = function (problem, message) {
      const brute = bruteForceCut(problem);
      const result = quilt.cut(problem);
      const again = quilt.cut(problem);

      c.check(result.labels instanceof Uint8Array, `${message}: label type`);
      c.check(
        result.labels.length === problem.width * problem.height,
        `${message}: label count`
      );
      c.check(
        result.energy === brute.bestEnergy,
        `${message}: globally minimal energy`
      );
      c.check(
        result.energy === brute.labelEnergy(result.labels),
        `${message}: reported energy matches the labelling`
      );
      c.check(
        Array.from(result.labels).every((v, i) => v === again.labels[i]),
        `${message}: deterministic labelling`
      );

      for (let p = 0; p < problem.width * problem.height; p++) {
        if (!problem.filled[p]) {
          c.check(
            result.labels[p] === 1,
            `${message}: unfilled cell forced new`
          );
        }

        if (problem.forceOld && problem.forceOld[p]) {
          c.check(
            result.labels[p] === 0,
            `${message}: forceOld terminal respected`
          );
        }

        if (problem.forceNew && problem.forceNew[p]) {
          c.check(
            result.labels[p] === 1,
            `${message}: forceNew terminal respected`
          );
        }
      }
    };

    // L-shaped filled region on a 3x3 grid
    {
      const width = 3;
      const height = 3;
      const cells = width * height;
      const filled = new Uint8Array([
        1, 1, 1,
        1, 0, 0,
        1, 0, 0,
      ]);
      const d = new Float64Array([
        4, 36, 9,
        16, 0, 0,
        1, 0, 0,
      ]);
      const { oldRGBA, newRGBA } = rgbaFromD(d, cells);
      const forceOld = new Uint8Array(cells);
      const forceNew = new Uint8Array(cells);
      forceOld[6] = 1;
      forceNew[2] = 1;

      assertOptimal(
        {
          width,
          height,
          oldRGBA,
          newRGBA,
          filled,
          forceOld,
          forceNew,
        },
        "L-shaped filled region"
      );
    }

    // 3x4 grid with a hole in the middle (12 cells, enumeration limit)
    {
      const width = 3;
      const height = 4;
      const cells = width * height;
      const filled = new Uint8Array(cells).fill(1);
      filled[4] = 0; // hole
      const d = new Float64Array([
        25, 4, 49,
        9, 0, 16,
        1, 64, 36,
        81, 100, 4,
      ]);
      const { oldRGBA, newRGBA } = rgbaFromD(d, cells);
      const forceOld = new Uint8Array(cells);
      const forceNew = new Uint8Array(cells);
      forceOld[0] = 1;
      forceOld[8] = 1;
      forceNew[11] = 1;

      assertOptimal(
        {
          width,
          height,
          oldRGBA,
          newRGBA,
          filled,
          forceOld,
          forceNew,
        },
        "grid with hole"
      );
    }

    // zero differences: every edge costs zero, terminals still hold
    {
      const width = 3;
      const height = 2;
      const cells = width * height;
      const filled = new Uint8Array(cells).fill(1);
      const oldRGBA = new Uint8ClampedArray(cells * 4);
      const newRGBA = new Uint8ClampedArray(cells * 4);

      for (let p = 0; p < cells; p++) {
        oldRGBA[p * 4] = 50;
        oldRGBA[p * 4 + 1] = 60;
        oldRGBA[p * 4 + 2] = 70;
        oldRGBA[p * 4 + 3] = 255;
        newRGBA[p * 4] = 50;
        newRGBA[p * 4 + 1] = 60;
        newRGBA[p * 4 + 2] = 70;
        newRGBA[p * 4 + 3] = 0; // alpha differs, RGB identical
      }

      const forceOld = new Uint8Array(cells);
      const forceNew = new Uint8Array(cells);
      forceOld[0] = 1;
      forceNew[cells - 1] = 1;

      const result = quilt.cut({
        width,
        height,
        oldRGBA,
        newRGBA,
        filled,
        forceOld,
        forceNew,
      });

      c.check(result.energy === 0, "identical RGB gives zero energy");
      c.check(
        result.labels[0] === 0 && result.labels[cells - 1] === 1,
        "zero-cost terminals still fixed"
      );
    }

    // alpha alone never changes the cost
    {
      const width = 2;
      const height = 2;
      const cells = 4;
      const filled = new Uint8Array(cells).fill(1);
      const oldRGBA = new Uint8ClampedArray(cells * 4);
      const newRGBA = new Uint8ClampedArray(cells * 4);

      for (let p = 0; p < cells; p++) {
        oldRGBA[p * 4] = 10;
        oldRGBA[p * 4 + 1] = 20;
        oldRGBA[p * 4 + 2] = 30;
        oldRGBA[p * 4 + 3] = 255;
        newRGBA[p * 4] = 10;
        newRGBA[p * 4 + 1] = 20;
        newRGBA[p * 4 + 2] = 30;
        newRGBA[p * 4 + 3] = p * 30;
      }

      const result = quilt.cut({
        width,
        height,
        oldRGBA,
        newRGBA,
        filled,
      });

      c.check(result.energy === 0, "alpha differences are ignored");
    }

    // terminal conflicts must throw, with no partial result
    {
      const cells = 4;
      const filled = new Uint8Array(cells).fill(1);
      const rgba = new Uint8ClampedArray(cells * 4);
      const forceOld = new Uint8Array(cells);
      const forceNew = new Uint8Array(cells);
      forceOld[1] = 1;
      forceNew[1] = 1;

      let threw = null;

      try {
        quilt.cut({
          width: 2,
          height: 2,
          oldRGBA: rgba,
          newRGBA: rgba,
          filled,
          forceOld,
          forceNew,
        });
      } catch (error) {
        threw = error;
      }

      c.check(threw instanceof RangeError, "force conflict throws RangeError");

      let unfilledThrew = null;
      forceNew[1] = 0;
      forceOld[1] = 1;
      filled[1] = 0;

      try {
        quilt.cut({
          width: 2,
          height: 2,
          oldRGBA: rgba,
          newRGBA: rgba,
          filled,
          forceOld,
          forceNew,
        });
      } catch (error) {
        unfilledThrew = error;
      }

      c.check(
        unfilledThrew instanceof RangeError,
        "forcing an unfilled cell old throws RangeError"
      );
    }

    // randomized small grids, every one compared with enumeration
    let state = 42;

    const rand = function () {
      state = lcgStep(state);

      return state / LCG_MODULUS;
    };

    for (let round = 0; round < 24; round++) {
      const width = 2 + Math.floor(rand() * 3);
      const height = 2;
      const cells = width * height;
      const filled = new Uint8Array(cells);
      const forceOld = new Uint8Array(cells);
      const forceNew = new Uint8Array(cells);
      const d = new Float64Array(cells);

      for (let p = 0; p < cells; p++) {
        filled[p] = rand() < 0.8 ? 1 : 0;
        const diff = filled[p] ? Math.floor(rand() * 11) : 0;
        d[p] = diff * diff;

        if (filled[p] && rand() < 0.3) forceOld[p] = 1;

        if (filled[p] && !forceOld[p] && rand() < 0.2) {
          forceNew[p] = 1;
        }
      }

      const { oldRGBA, newRGBA } = rgbaFromD(d, cells);
      const problem = {
        width,
        height,
        oldRGBA,
        newRGBA,
        filled,
        forceOld,
        forceNew,
      };
      const brute = bruteForceCut(problem);
      const result = quilt.cut(problem);

      c.check(
        result.energy === brute.bestEnergy,
        `random grid ${round}: optimal energy`
      );
      c.check(
        result.energy === brute.labelEnergy(result.labels),
        `random grid ${round}: energy consistent with labels`
      );
    }

    // the per-cell greedy degeneration is strictly worse on this instance
    {
      const width = 3;
      const height = 4;
      const cells = width * height;
      const d = new Float64Array([
        81, 1, 100,
        36, 16, 0,
        100, 100, 81,
        0, 1, 16,
      ]);
      const filled = new Uint8Array([
        1, 1, 1,
        1, 1, 0,
        1, 1, 1,
        1, 1, 1,
      ]);
      const forceOld = new Uint8Array(cells);
      const forceNew = new Uint8Array(cells);
      forceOld[4] = 1;
      forceNew[1] = 1;

      // encode d exactly: red channel difference of sqrt(d), green/blue zero
      const oldRGBA = new Uint8ClampedArray(cells * 4);
      const newRGBA = new Uint8ClampedArray(cells * 4);

      for (let p = 0; p < cells; p++) {
        oldRGBA[p * 4] = Math.round(Math.sqrt(d[p]));
        oldRGBA[p * 4 + 3] = 255;
        newRGBA[p * 4 + 3] = 255;
      }

      const problem = {
        width,
        height,
        oldRGBA,
        newRGBA,
        filled,
        forceOld,
        forceNew,
      };
      const brute = bruteForceCut(problem);
      const result = quilt.cut(problem);
      const greedy = greedyCut(
        width,
        height,
        brute.d,
        filled,
        forceOld,
        forceNew
      );

      c.check(
        result.energy === brute.bestEnergy,
        "degenerate case: min cut reaches the enumerated optimum"
      );
      c.check(
        greedy.energy > result.energy,
        "row-major greedy loses to the global minimum cut"
      );
      c.check(
        brute.labelEnergy(greedy.labels) === greedy.energy,
        "the greedy labelling itself is a legal labelling"
      );
    }

    // full-block support: a large cut must run without enumeration
    {
      const width = 32;
      const height = 32;
      const cells = width * height;
      const filled = new Uint8Array(cells).fill(1);
      const oldRGBA = new Uint8ClampedArray(cells * 4);
      const newRGBA = new Uint8ClampedArray(cells * 4);

      let localState = 7;

      for (let p = 0; p < cells; p++) {
        localState = lcgStep(localState);
        const value = Math.floor((localState / LCG_MODULUS) * 256);
        oldRGBA[p * 4] = value;
        oldRGBA[p * 4 + 3] = 255;
        newRGBA[p * 4] = 255 - value;
        newRGBA[p * 4 + 3] = 255;
      }

      const forceOld = new Uint8Array(cells);
      const forceNew = new Uint8Array(cells);
      forceOld[0] = 1;
      forceNew[cells - 1] = 1;

      const result = quilt.cut({
        width,
        height,
        oldRGBA,
        newRGBA,
        filled,
        forceOld,
        forceNew,
      });

      c.check(
        result.labels.length === cells &&
          result.labels[0] === 0 &&
          result.labels[cells - 1] === 1,
        "large block cut honours both terminals"
      );
      c.check(
        Number.isFinite(result.energy) && result.energy > 0,
        "large block cut returns a finite energy without enumeration"
      );
    }
  },

  // wrapping, clipped edges and pixel/provenance consistency
  periodic(c) {
    const source = makeSource(23, 8, 8);

    const base = {
      patchWidth: 5,
      patchHeight: 4,
      overlapX: 2,
      overlapY: 1,
      tolerance: 0.4,
      seed: 777,
    };

    const assertPixelConsistency = function (result, options, message) {
      c.check(
        result.data instanceof Uint8ClampedArray,
        `${message}: Uint8ClampedArray data`
      );
      c.check(
        result.provenance instanceof Int32Array,
        `${message}: Int32Array provenance`
      );
      c.check(
        result.data.length === result.width * result.height * 4,
        `${message}: data size`
      );
      c.check(
        result.provenance.length === result.width * result.height,
        `${message}: provenance size`
      );

      for (let cell = 0; cell < result.width * result.height; cell++) {
        const sourceIndex = result.provenance[cell];

        c.check(
          sourceIndex >= 0 &&
            sourceIndex < source.width * source.height,
          `${message}: every cell has a source provenance index`
        );

        for (let k = 0; k < 4; k++) {
          c.check(
            result.data[cell * 4 + k] ===
              source.data[sourceIndex * 4 + k],
            `${message}: RGBA group copied from the provenance pixel`
          );
        }
      }

      let crossesX = false;
      let crossesY = false;

      for (const placement of result.placements) {
        const localWidth = options.wrap
          ? options.patchWidth
          : Math.min(options.patchWidth, result.width - placement.x);
        const localHeight = options.wrap
          ? options.patchHeight
          : Math.min(options.patchHeight, result.height - placement.y);

        c.check(
          placement.labels.length === localWidth * localHeight,
          `${message}: local label size`
        );

        if (placement.x + options.patchWidth > result.width) crossesX = true;
        if (placement.y + options.patchHeight > result.height) crossesY = true;
      }

      c.check(crossesX, `${message}: a placement crosses the right edge`);
      c.check(crossesY, `${message}: a placement crosses the bottom edge`);
    };

    // clipped, non-wrapping synthesis
    const clipped = quilt.synthesize(source, {
      ...base,
      width: 13,
      height: 11,
      wrap: false,
    });

    assertPixelConsistency(
      clipped,
      { ...base, width: 13, height: 11, wrap: false },
      "clipped"
    );

    // wrapping synthesis with blocks spanning both axes
    const wrapped = quilt.synthesize(source, {
      ...base,
      width: 12,
      height: 12,
      wrap: true,
    });

    assertPixelConsistency(
      wrapped,
      { ...base, width: 12, height: 12, wrap: true },
      "wrapped"
    );

    const crossesBoth = wrapped.placements.some(
      (placement) =>
        placement.x + base.patchWidth > wrapped.width &&
        placement.y + base.patchHeight > wrapped.height
    );

    c.check(
      crossesBoth,
      "wrapped synthesis has a block crossing both axes at once"
    );

    // wrap placement labels always describe the full, unclipped block
    const fullBlockLabels = wrapped.placements.every(
      (placement) =>
        placement.labels.length === base.patchWidth * base.patchHeight
    );

    c.check(fullBlockLabels, "wrapped blocks keep their full local size");

    // heavy overlap in wrap mode creates placements covering only cells
    // already filled: the local centre must then be forced to the new label
    const denseOptions = {
      width: 8,
      height: 8,
      patchWidth: 7,
      patchHeight: 7,
      overlapX: 5,
      overlapY: 5,
      tolerance: 0,
      seed: 31,
      wrap: true,
    };
    const dense = quilt.synthesize(source, denseOptions);
    const denseReports = enumerateCandidates(dense, source, denseOptions);
    let sawAllFilled = false;

    for (const report of denseReports) {
      const placement = report.placement;

      if (placement.skipped) {
        // a skipped placement only happens when the centre is itself forced
        // old; it carries no cut and an all-old label array
        c.check(
          placement.energy === 0 &&
            placement.labels.every((label) => label === 0),
          "skipped placements carry no cut and no labels"
        );
        continue;
      }

      if (report.allFilled) {
        sawAllFilled = true;
        const centre =
          Math.floor(denseOptions.patchHeight / 2) *
            denseOptions.patchWidth +
          Math.floor(denseOptions.patchWidth / 2);

        c.check(
          placement.labels[centre] === 1,
          "all-filled blocks force the centre cell to the new label"
        );
      }
    }

    c.check(sawAllFilled, "dense wrapping reaches an all-filled placement");

    for (let cell = 0; cell < dense.width * dense.height; cell++) {
      c.check(
        dense.provenance[cell] >= 0,
        "dense wrapping still fills every output cell"
      );
    }

    // the returned buffers are independent of the source
    const before = Array.from(source.data);
    const independent = quilt.synthesize(source, {
      ...base,
      width: 9,
      height: 9,
      wrap: false,
    });
    independent.data[0] = independent.data[0] === 0 ? 1 : 0;
    independent.provenance[0] = -99;

    c.check(
      source.data.every((value, index) => value === before[index]),
      "mutating the result does not touch the source"
    );
  },

  // replay through placements, do/render, repeatability and validation
  replay(c) {
    const source = makeSource(51, 8, 6);
    const options = {
      width: 14,
      height: 12,
      patchWidth: 5,
      patchHeight: 4,
      overlapX: 2,
      overlapY: 2,
      tolerance: 0.3,
      seed: 2026,
      wrap: false,
    };

    const result = quilt.synthesize(source, options);

    for (const placement of result.placements) {
      for (const key of [
        "x",
        "y",
        "sourceX",
        "sourceY",
        "candidateCount",
        "score",
        "energy",
        "skipped",
        "labels",
      ]) {
        c.check(
          Object.prototype.hasOwnProperty.call(placement, key),
          `placement record exposes ${key}`
        );
      }
    }

    const replayed = replayPlacements(result, source, options);

    c.check(
      Array.from(replayed.data).every((v, i) => v === result.data[i]),
      "placements replay the final image"
    );
    c.check(
      Array.from(replayed.provenance).every(
        (v, i) => v === result.provenance[i]
      ),
      "placements replay the provenance"
    );

    const wrapOptions = {
      width: 12,
      height: 12,
      patchWidth: 5,
      patchHeight: 4,
      overlapX: 2,
      overlapY: 1,
      tolerance: 0.3,
      seed: 2027,
      wrap: true,
    };
    const wrapped = quilt.synthesize(source, wrapOptions);
    const replayedWrapped = replayPlacements(
      wrapped,
      source,
      wrapOptions
    );

    c.check(
      Array.from(replayedWrapped.data).every(
        (v, i) => v === wrapped.data[i]
      ),
      "wrapped placements replay the final image"
    );

    // generator.do matches the standalone call, with no blending
    const generator = tgen.init(options.width, options.height, "clamped");
    const seedBeforeFirst = generator.calc.seed;
    const returned = generator.do("quilt", {
      source,
      options: { ...options },
    });

    c.check(returned === generator, "do returns the generator for chaining");
    c.check(
      generator.calc.seed === seedBeforeFirst,
      "calc.seed unchanged when quilt is the first effect"
    );
    c.check(
      Array.from(generator.texture.data).every(
        (value, index) => value === result.data[index]
      ),
      "do writes the standalone synthesis pixels"
    );

    // quilt keeps the seed sequence of surrounding effects intact
    const seeded = tgen.init(options.width, options.height, "clamped");
    seeded.do("noise", {
      seed: 9876,
      mode: "monochrome",
      blend: "opacity",
      channels: [255, 255, 255],
      opacity: 255,
    });
    const seedBeforeQuilt = seeded.calc.seed;
    seeded.do("quilt", { source, options: { ...options } });

    c.check(
      seeded.calc.seed === seedBeforeQuilt,
      "calc.seed unchanged after quilt following another effect"
    );

    // JSON round-trip replay through render()
    const config = JSON.parse(JSON.stringify(generator.params("quilt-demo")));
    const stored = config.items[0][2];

    c.check(
      Array.isArray(stored.source.data),
      "stored TypedArray source data becomes a plain array"
    );
    c.check(
      stored.options !== undefined &&
        stored.options.seed === options.seed,
      "stored options survive JSON serialization"
    );
    c.check(
      !("data" in stored) && !("labels" in stored),
      "synthesis results and cut state are not stored"
    );

    const rendered = tgen
      .init(options.width, options.height, "clamped")
      .render(config);

    c.check(
      Array.from(rendered.texture.data).every(
        (value, index) => value === result.data[index]
      ),
      "render() reproduces the standalone synthesis"
    );

    // same seed is repeatable; generators stay independent
    const a = tgen.init(options.width, options.height, "clamped");
    const b = tgen.init(options.width, options.height, "clamped");
    const aAlone = tgen.init(options.width, options.height, "clamped");

    a.do("quilt", { source, options: { ...options } });
    b.do("quilt", {
      source,
      options: { ...options, seed: options.seed + 1 },
    });
    aAlone.do("quilt", { source, options: { ...options } });

    c.check(
      Array.from(a.texture.data).some(
        (value, index) => value !== b.texture.data[index]
      ),
      "different quilt seeds produce different textures"
    );
    c.check(
      Array.from(a.texture.data).every(
        (value, index) => value === aAlone.texture.data[index]
      ),
      "interleaved generators do not influence each other"
    );

    const once = tgen
      .init(options.width, options.height, "clamped")
      .do("quilt", { source, options: { ...options } });
    const twice = tgen
      .init(options.width, options.height, "clamped")
      .do("quilt", { source, options: { ...options } });

    c.check(
      Array.from(once.texture.data).every(
        (value, index) => value === twice.texture.data[index]
      ),
      "the same seed repeats through real do() calls"
    );

    // validation: illegal input throws TypeError/RangeError, never partial
    const valid = { ...options };
    const invalidCases = [
      [{ width: 1, height: 4, data: new Uint8ClampedArray(16) }, valid],
      [
        { width: 65, height: 4, data: new Uint8ClampedArray(65 * 4 * 4) },
        valid,
      ],
      [
        {
          width: 4,
          height: 4,
          data: new Uint8ClampedArray(4 * 4 * 4 - 1),
        },
        valid,
      ],
      [
        (() => {
          const bad = makeSource(1, 4, 4);
          bad.data[3] = 256;

          return bad;
        })(),
        valid,
      ],
      [source, { ...valid, width: 5 }],
      [source, { ...valid, height: 129 }],
      [source, { ...valid, patchWidth: 2 }],
      [source, { ...valid, patchWidth: source.width + 1 }],
      [source, { ...valid, patchWidth: valid.width }],
      [source, { ...valid, overlapX: 0 }],
      [source, { ...valid, overlapX: valid.patchWidth }],
      [source, { ...valid, tolerance: -0.1 }],
      [source, { ...valid, tolerance: Number.NaN }],
      [source, { ...valid, seed: -1 }],
      [source, { ...valid, seed: 4294967296 }],
      [source, { ...valid, wrap: "true" }],
      [null, valid],
      [source, null],
    ];

    invalidCases.forEach(function (entry, index) {
      let threw = null;

      try {
        quilt.synthesize(entry[0], entry[1]);
      } catch (error) {
        threw = error;
      }

      c.check(
        threw instanceof TypeError || threw instanceof RangeError,
        `invalid case ${index} throws TypeError or RangeError`
      );
    });

    let cutThrew = null;

    try {
      quilt.cut({ width: 2, height: 2 });
    } catch (error) {
      cutThrew = error;
    }

    c.check(
      cutThrew instanceof TypeError || cutThrew instanceof RangeError,
      "malformed cut problem throws"
    );

    // README documents the small example image workflow
    const readme = fs.readFileSync(
      path.join(__dirname, "..", "README.md"),
      "utf8"
    );

    c.check(
      readme.includes("quilt") && readme.includes("synthesize"),
      "README documents the quilt synthesis example"
    );
  },
};
