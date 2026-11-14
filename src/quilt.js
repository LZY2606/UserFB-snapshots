"use strict";

/**
 * Image quilting: texture synthesis from a small example image.
 *
 * Patches are picked from the source by the RGB squared error of the
 * already filled overlap pixels, one candidate is chosen with a local LCG,
 * and a single global binary minimum cut (max flow) decides which pixels of
 * the new patch replace existing ones.
 *
 * CommonJS only, Node built-ins only.
 */

const LCG_MULTIPLIER = 1664525;
const LCG_INCREMENT = 1013904223;
const LCG_MODULUS = 4294967296; // 2^32
const LCG_MASK = 0xffffffff;

const isInteger = function (value) {
  return typeof value === "number" && Number.isInteger(value);
};

const isFiniteNumber = function (value) {
  return typeof value === "number" && Number.isFinite(value);
};

const isIndexable = function (value) {
  return (
    Array.isArray(value) ||
    (ArrayBuffer.isView(value) && !(value instanceof DataView))
  );
};

const isRGBAStorage = function (value) {
  return isIndexable(value) && value.length > 0;
};

const validateImage = function (image, name) {
  if (image === null || typeof image !== "object") {
    throw new TypeError(`${name} must be an object`);
  }

  if (!isInteger(image.width) || !isInteger(image.height)) {
    throw new TypeError(`${name}.width and ${name}.height must be integers`);
  }

  if (image.width < 2 || image.width > 64 || image.height < 2 || image.height > 64) {
    throw new RangeError(`${name} sides must be between 2 and 64`);
  }

  if (!isRGBAStorage(image.data)) {
    throw new TypeError(`${name}.data must be an array or a TypedArray`);
  }

  if (image.data.length !== image.width * image.height * 4) {
    throw new RangeError(`${name}.data length must match width * height * 4`);
  }

  for (let i = 0; i < image.data.length; i++) {
    if (!isInteger(image.data[i]) || image.data[i] < 0 || image.data[i] > 255) {
      throw new RangeError(`${name}.data components must be integers from 0 to 255`);
    }
  }
};

const validateOptions = function (source, options) {
  if (options === null || typeof options !== "object") {
    throw new TypeError("options must be an object");
  }

  const {
    width,
    height,
    patchWidth,
    patchHeight,
    overlapX,
    overlapY,
    tolerance,
    seed,
    wrap,
  } = options;

  if (!isInteger(width) || !isInteger(height)) {
    throw new TypeError("options.width and options.height must be integers");
  }

  if (width < 6 || width > 128 || height < 6 || height > 128) {
    throw new RangeError("output sides must be between 6 and 128");
  }

  if (!isInteger(patchWidth) || !isInteger(patchHeight)) {
    throw new TypeError("patchWidth and patchHeight must be integers");
  }

  if (patchWidth < 3 || patchHeight < 3) {
    throw new RangeError("patch sides must be at least 3");
  }

  if (patchWidth > source.width || patchHeight > source.height) {
    throw new RangeError("patch sides must not exceed the source sides");
  }

  if (patchWidth >= width || patchHeight >= height) {
    throw new RangeError("patch sides must be strictly smaller than the output sides");
  }

  if (!isInteger(overlapX) || !isInteger(overlapY)) {
    throw new TypeError("overlapX and overlapY must be integers");
  }

  if (
    overlapX < 1 ||
    overlapX > patchWidth - 1 ||
    overlapY < 1 ||
    overlapY > patchHeight - 1
  ) {
    throw new RangeError("overlaps must be between 1 and the patch side minus 1");
  }

  if (!isFiniteNumber(tolerance) || tolerance < 0) {
    throw new RangeError("tolerance must be a finite non-negative number");
  }

  if (!isInteger(seed) || seed < 0 || seed > LCG_MASK) {
    throw new RangeError("seed must be a uint32 integer");
  }

  if (typeof wrap !== "boolean") {
    throw new TypeError("wrap must be a boolean");
  }
};

/**
 * Validate a cut problem. Every array describes the same local rectangle.
 */
const validateProblem = function (problem) {
  if (problem === null || typeof problem !== "object") {
    throw new TypeError("problem must be an object");
  }

  const { width, height, oldRGBA, newRGBA, filled, forceOld, forceNew } = problem;

  if (!isInteger(width) || !isInteger(height) || width < 1 || height < 1) {
    throw new RangeError("problem.width and problem.height must be positive integers");
  }

  const cells = width * height;

  if (
    !isRGBAStorage(oldRGBA) ||
    !isRGBAStorage(newRGBA) ||
    oldRGBA.length !== cells * 4 ||
    newRGBA.length !== cells * 4
  ) {
    throw new RangeError("oldRGBA and newRGBA must hold width * height RGBA pixels");
  }

  if (!isIndexable(filled)) {
    throw new TypeError("filled must be an array or a TypedArray");
  }

  if (filled.length !== cells) {
    throw new RangeError("filled length must match width * height");
  }

  const checkMask = function (mask, name) {
    if (mask === undefined || mask === null) {
      return new Uint8Array(cells);
    }

    if (!isIndexable(mask)) {
      throw new TypeError(`${name} must be an array or a TypedArray`);
    }

    if (mask.length !== cells) {
      throw new RangeError(`${name} length must match width * height`);
    }

    const out = new Uint8Array(cells);

    for (let i = 0; i < cells; i++) {
      out[i] = mask[i] ? 1 : 0;
    }

    return out;
  };

  const oldMask = checkMask(forceOld, "forceOld");
  const newMask = checkMask(forceNew, "forceNew");

  for (let i = 0; i < cells; i++) {
    if (oldMask[i] && newMask[i]) {
      throw new RangeError(`forceOld and forceNew conflict at cell ${i}`);
    }
  }

  const filledFlags = new Uint8Array(cells);

  for (let i = 0; i < cells; i++) {
    filledFlags[i] = filled[i] ? 1 : 0;
  }

  // unfilled cells are terminal "new" pixels, so they cannot be forced old
  for (let i = 0; i < cells; i++) {
    if (!filledFlags[i] && oldMask[i]) {
      throw new RangeError(`unfilled cell ${i} cannot be forced old`);
    }
  }

  return { cells, filledFlags, oldMask, newMask };
};

/**
 * Binary minimum cut on a 4-connected grid with terminal constraints.
 *
 * Label 0 = keep the old pixel, label 1 = take the new pixel.
 * Edge (p, q) with differing labels costs d[p] + d[q], where d is the RGB
 * squared difference (0 for unfilled cells). Terminals are encoded with
 * infinite-capacity arcs to the source (force old) and sink (force new).
 *
 * Polynomial max flow (Dinic), no enumeration of label combinations.
 */
const minCut = function (width, height, d, filledFlags, oldMask, newMask) {
  const cells = width * height;
  const source = cells;
  const sink = cells + 1;
  const nodeCount = cells + 2;
  const INFINITY = 1e15;

  const graph = new Array(nodeCount);

  for (let i = 0; i < nodeCount; i++) {
    graph[i] = [];
  }

  const addEdge = function (from, to, capacity) {
    graph[from].push({ to, capacity, reverse: graph[to].length });
    graph[to].push({ to: from, capacity: 0, reverse: graph[from].length - 1 });
  };

  // symmetric pairwise costs: an undirected edge of capacity w is represented
  // by two directed arcs of capacity w
  const addPairEdge = function (p, q, capacity) {
    if (capacity === 0) {
      return;
    }

    graph[p].push({ to: q, capacity, reverse: graph[q].length });
    graph[q].push({ to: p, capacity, reverse: graph[p].length - 1 });
  };

  for (let p = 0; p < cells; p++) {
    if (oldMask[p]) {
      addEdge(source, p, INFINITY);
    } else if (newMask[p] || !filledFlags[p]) {
      addEdge(p, sink, INFINITY);
    }
  }

  for (let y = 0; y < height; y++) {
    for (let x = 0; x < width; x++) {
      const p = y * width + x;

      if (x + 1 < width) {
        const q = p + 1;
        addPairEdge(p, q, d[p] + d[q]);
      }

      if (y + 1 < height) {
        const q = p + width;
        addPairEdge(p, q, d[p] + d[q]);
      }
    }
  }

  // Dinic's algorithm, iterative so a full block cannot overflow the stack
  const level = new Int32Array(nodeCount);
  const queue = new Int32Array(nodeCount);
  let flow = 0;

  const bfs = function () {
    level.fill(-1);
    level[source] = 0;
    let head = 0;
    let tail = 0;
    queue[tail++] = source;

    while (head < tail) {
      const node = queue[head++];
      const edges = graph[node];

      for (let i = 0; i < edges.length; i++) {
        const edge = edges[i];

        if (edge.capacity > 0 && level[edge.to] < 0) {
          level[edge.to] = level[node] + 1;
          queue[tail++] = edge.to;
        }
      }
    }

    return level[sink] >= 0;
  };

  // push one blocking flow with explicit path stacks (current arc)
  const blockingFlow = function () {
    const ptr = new Int32Array(nodeCount);
    const nodeStack = new Int32Array(nodeCount);
    const edgeStack = new Int32Array(nodeCount);
    let top = 0;
    nodeStack[0] = source;

    while (top >= 0) {
      const node = nodeStack[top];

      if (node === sink) {
        let pushed = INFINITY;

        for (let i = 0; i < top; i++) {
          const edge = graph[nodeStack[i]][edgeStack[i]];
          pushed = Math.min(pushed, edge.capacity);
        }

        let backtrackTo = top;

        for (let i = 0; i < top; i++) {
          const edge = graph[nodeStack[i]][edgeStack[i]];
          edge.capacity -= pushed;
          graph[edge.to][edge.reverse].capacity += pushed;

          if (edge.capacity === 0 && backtrackTo === top) {
            backtrackTo = i;
          }
        }

        flow += pushed;
        top = backtrackTo;
        continue;
      }

      const edges = graph[node];

      while (
        ptr[node] < edges.length &&
        (edges[ptr[node]].capacity <= 0 ||
          level[edges[ptr[node]].to] !== level[node] + 1)
      ) {
        ptr[node]++;
      }

      if (ptr[node] === edges.length) {
        level[node] = -1;
        top--;
        continue;
      }

      edgeStack[top] = ptr[node];
      nodeStack[top + 1] = edges[ptr[node]].to;
      top++;
    }
  };

  while (bfs()) {
    blockingFlow();
  }

  // reachable from the source after the max flow: source side = label 0
  const labels = new Uint8Array(cells);
  let head = 0;
  let tail = 0;
  const reached = new Uint8Array(nodeCount);
  reached[source] = 1;
  queue[tail++] = source;

  while (head < tail) {
    const node = queue[head++];
    const edges = graph[node];

    for (let i = 0; i < edges.length; i++) {
      const edge = edges[i];

      if (edge.capacity > 0 && !reached[edge.to]) {
        reached[edge.to] = 1;
        queue[tail++] = edge.to;
      }
    }
  }

  for (let i = 0; i < cells; i++) {
    labels[i] = reached[i] ? 0 : 1;
  }

  // the cut value equals the max flow; recompute the stated energy directly
  // from the labels so rounding or INF arcs never leak into the result
  let energy = 0;

  for (let y = 0; y < height; y++) {
    for (let x = 0; x < width; x++) {
      const p = y * width + x;

      if (x + 1 < width) {
        const q = p + 1;

        if (labels[p] !== labels[q]) {
          energy += d[p] + d[q];
        }
      }

      if (y + 1 < height) {
        const q = p + width;

        if (labels[p] !== labels[q]) {
          energy += d[p] + d[q];
        }
      }
    }
  }

  return { labels, energy };
};

/**
 * Solve one local cut problem.
 *
 * problem = {width, height, oldRGBA, newRGBA, filled, forceOld, forceNew}
 * returns {labels, energy} with labels as a Uint8Array (0 old, 1 new).
 */
const cut = function (problem) {
  const { width, height } = problem || {};
  const { cells, filledFlags, oldMask, newMask } = validateProblem(problem);

  const d = new Float64Array(cells);

  for (let p = 0; p < cells; p++) {
    if (!filledFlags[p]) {
      d[p] = 0;
      continue;
    }

    const offset = p * 4;
    const dr = problem.oldRGBA[offset] - problem.newRGBA[offset];
    const dg = problem.oldRGBA[offset + 1] - problem.newRGBA[offset + 1];
    const db = problem.oldRGBA[offset + 2] - problem.newRGBA[offset + 2];
    // alpha never participates in the cut cost
    d[p] = dr * dr + dg * dg + db * db;
  }

  const result = minCut(width, height, d, filledFlags, oldMask, newMask);

  // terminal constraints must hold in the returned labelling
  for (let i = 0; i < cells; i++) {
    if (oldMask[i] && result.labels[i] !== 0) {
      throw new RangeError("cut could not satisfy forceOld");
    }

    if ((newMask[i] || !filledFlags[i]) && result.labels[i] !== 1) {
      throw new RangeError("cut could not satisfy forceNew");
    }
  }

  return result;
};

/**
 * RGB squared difference at one source and one output pixel.
 */
const pixelError = function (sourceData, sourceOffset, outputData, outputOffset) {
  const dr = sourceData[sourceOffset] - outputData[outputOffset];
  const dg = sourceData[sourceOffset + 1] - outputData[outputOffset + 1];
  const db = sourceData[sourceOffset + 2] - outputData[outputOffset + 2];

  return dr * dr + dg * dg + db * db;
};

/**
 * Synthesize a larger texture from a small example image.
 *
 * Returns {width, height, data, provenance, placements} with independent
 * buffers; the source image is never modified.
 */
const synthesize = function (source, options) {
  validateImage(source, "source");

  if (options === null || typeof options !== "object") {
    throw new TypeError("options must be an object");
  }

  validateOptions(source, options);

  const outWidth = options.width;
  const outHeight = options.height;
  const patchWidth = options.patchWidth;
  const patchHeight = options.patchHeight;
  const overlapX = options.overlapX;
  const overlapY = options.overlapY;
  const tolerance = options.tolerance;
  const wrap = options.wrap;

  const stepX = patchWidth - overlapX;
  const stepY = patchHeight - overlapY;

  const sourceData = source.data;
  const sourceWidth = source.width;
  const sourceHeight = source.height;
  const maxSourceX = sourceWidth - patchWidth;
  const maxSourceY = sourceHeight - patchHeight;

  // independent output buffers, initially empty
  const data = new Uint8ClampedArray(outWidth * outHeight * 4);
  const provenance = new Int32Array(outWidth * outHeight).fill(-1);
  const filled = new Uint8Array(outWidth * outHeight);
  const placements = [];

  // local LCG, strictly one update per placed patch
  let lcgState = options.seed >>> 0;

  const outputIndex = function (x, y) {
    return (y * outWidth + x) * 4;
  };

  const anchorLoop = function (callback) {
    for (let anchorY = 0; anchorY < outHeight; anchorY += stepY) {
      for (let anchorX = 0; anchorX < outWidth; anchorX += stepX) {
        callback(anchorX, anchorY);
      }
    }
  };

  anchorLoop(function (anchorX, anchorY) {
    // effective local rectangle, clipped at the output edges unless wrapping
    const localWidth = wrap
      ? patchWidth
      : Math.min(patchWidth, outWidth - anchorX);
    const localHeight = wrap
      ? patchHeight
      : Math.min(patchHeight, outHeight - anchorY);

    if (localWidth <= 0 || localHeight <= 0) {
      return;
    }

    // map local (lx, ly) to output coordinates
    const mapX = function (lx) {
      return wrap ? (anchorX + lx) % outWidth : anchorX + lx;
    };

    const mapY = function (ly) {
      return wrap ? (anchorY + ly) % outHeight : anchorY + ly;
    };

    const localCells = localWidth * localHeight;
    const localFilled = new Uint8Array(localCells);
    let anyFilled = false;
    let allFilled = true;

    for (let ly = 0; ly < localHeight; ly++) {
      for (let lx = 0; lx < localWidth; lx++) {
        const cell = ly * localWidth + lx;
        const outCell = mapY(ly) * outWidth + mapX(lx);

        if (filled[outCell]) {
          localFilled[cell] = 1;
          anyFilled = true;
        } else {
          allFilled = false;
        }
      }
    }

    // score every source position that fits a complete patch
    let minError = Infinity;
    const errors = new Float64Array((maxSourceY + 1) * (maxSourceX + 1));
    let candidateIndex = 0;

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

              const outOffset = outputIndex(mapX(lx), mapY(ly));
              const sourceOffset =
                ((sourceY + ly) * sourceWidth + (sourceX + lx)) * 4;

              // each filled local cell counts exactly once; corners are not
              // counted twice and empty cells are never treated as black
              error += pixelError(sourceData, sourceOffset, data, outOffset);
            }
          }
        }

        errors[candidateIndex++] = error;

        if (error < minError) {
          minError = error;
        }
      }
    }

    // build the candidate pool, ordered by source y, then x
    const pool = [];
    const threshold =
      minError === 0 ? 0 : minError * (1 + tolerance);

    candidateIndex = 0;

    for (let sourceY = 0; sourceY <= maxSourceY; sourceY++) {
      for (let sourceX = 0; sourceX <= maxSourceX; sourceX++) {
        const error = errors[candidateIndex++];
        const accepted =
          minError === 0 ? error === 0 : error <= threshold;

        if (accepted) {
          pool.push({ sourceX, sourceY, error });
        }
      }
    }

    // one LCG update per patch, including the first one
    lcgState = (LCG_MULTIPLIER * lcgState + LCG_INCREMENT) % LCG_MODULUS;
    lcgState = lcgState >>> 0;
    const chosenIndex = Math.floor((lcgState / LCG_MODULUS) * pool.length);
    const chosen = pool[chosenIndex];

    // build the local cut problem from the current output and the patch
    const oldRGBA = new Uint8ClampedArray(localCells * 4);
    const newRGBA = new Uint8ClampedArray(localCells * 4);
    const forceOld = new Uint8Array(localCells);
    const forceNew = new Uint8Array(localCells);

    // output cells covered by this placement (modular coordinates for wrap)
    const coverage = new Uint8Array(outWidth * outHeight);

    for (let ly = 0; ly < localHeight; ly++) {
      for (let lx = 0; lx < localWidth; lx++) {
        coverage[mapY(ly) * outWidth + mapX(lx)] = 1;
      }
    }

    for (let ly = 0; ly < localHeight; ly++) {
      for (let lx = 0; lx < localWidth; lx++) {
        const cell = ly * localWidth + lx;
        const offset = cell * 4;
        const outX = mapX(lx);
        const outY = mapY(ly);
        const outOffset = outputIndex(outX, outY);
        const sourceOffset =
          ((chosen.sourceY + ly) * sourceWidth + (chosen.sourceX + lx)) * 4;

        oldRGBA[offset] = data[outOffset];
        oldRGBA[offset + 1] = data[outOffset + 1];
        oldRGBA[offset + 2] = data[outOffset + 2];
        oldRGBA[offset + 3] = data[outOffset + 3];

        newRGBA[offset] = sourceData[sourceOffset];
        newRGBA[offset + 1] = sourceData[sourceOffset + 1];
        newRGBA[offset + 2] = sourceData[sourceOffset + 2];
        newRGBA[offset + 3] = sourceData[sourceOffset + 3];

        if (localFilled[cell]) {
          // a filled pixel with an already filled output neighbour outside
          // this block's coverage must keep the old pixel; modular
          // neighbour coordinates when wrapping
          const neighbours = [
            [outX - 1, outY],
            [outX + 1, outY],
            [outX, outY - 1],
            [outX, outY + 1],
          ];

          for (const [rawNX, rawNY] of neighbours) {
            let nx = rawNX;
            let ny = rawNY;

            if (wrap) {
              nx = (nx + outWidth) % outWidth;
              ny = (ny + outHeight) % outHeight;
            } else if (nx < 0 || nx >= outWidth || ny < 0 || ny >= outHeight) {
              continue;
            }

            const neighbourCell = ny * outWidth + nx;

            if (filled[neighbourCell] && !coverage[neighbourCell]) {
              forceOld[cell] = 1;
              break;
            }
          }
        } else {
          // uncovered output pixels are always taken from the new patch
          forceNew[cell] = 1;
        }
      }
    }

    let skipped = false;

    if (allFilled) {
      // nothing new to fill: force the local centre to the new pixel so the
      // cut still has a meaningful boundary
      const centreX = Math.floor(localWidth / 2);
      const centreY = Math.floor(localHeight / 2);
      const centreCell = centreY * localWidth + centreX;

      if (forceOld[centreCell]) {
        skipped = true;
      } else {
        forceNew[centreCell] = 1;
      }
    }

    const placement = {
      x: anchorX,
      y: anchorY,
      sourceX: chosen.sourceX,
      sourceY: chosen.sourceY,
      candidateCount: pool.length,
      score: chosen.error,
      energy: 0,
      skipped,
      labels: new Uint8Array(localCells),
    };

    if (!skipped) {
      const result = cut({
        width: localWidth,
        height: localHeight,
        oldRGBA,
        newRGBA,
        filled: localFilled,
        forceOld,
        forceNew,
      });

      placement.labels = result.labels;
      placement.energy = result.energy;

      // copy whole RGBA groups from one source pixel per output cell
      for (let ly = 0; ly < localHeight; ly++) {
        for (let lx = 0; lx < localWidth; lx++) {
          const cell = ly * localWidth + lx;

          if (result.labels[cell] !== 1) {
            continue;
          }

          const outX = mapX(lx);
          const outY = mapY(ly);
          const outCell = outY * outWidth + outX;
          const outOffset = outCell * 4;
          const sourcePixelY = chosen.sourceY + ly;
          const sourcePixelX = chosen.sourceX + lx;
          const sourceOffset =
            (sourcePixelY * sourceWidth + sourcePixelX) * 4;

          data[outOffset] = sourceData[sourceOffset];
          data[outOffset + 1] = sourceData[sourceOffset + 1];
          data[outOffset + 2] = sourceData[sourceOffset + 2];
          data[outOffset + 3] = sourceData[sourceOffset + 3];
          provenance[outCell] = sourcePixelY * sourceWidth + sourcePixelX;
          filled[outCell] = 1;
        }
      }
    }

    placements.push(placement);
  });

  return { width: outWidth, height: outHeight, data, provenance, placements };
};

module.exports = {
  cut,
  synthesize,
};
