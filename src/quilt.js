"use strict";

/**
 * tgen quilt - build a larger texture from a small example image.
 *
 * Rectangular patches are copied out of the source image. Where a patch
 * overlaps pixels that are already filled, a global binary minimum cut decides
 * which side of the seam keeps its pixels. The horizontal and the vertical
 * overlaps are part of the same graph - the seam is never assembled from two
 * independent one dimensional cuts and the colors are never averaged.
 *
 * Only CommonJS and Node.js built-in modules are used.
 */

const UINT32 = 4294967296;
const INT32_MAX = 2147483647;

function position(x, y, width) {
  return y * width + x;
}

function isInteger(value) {
  return typeof value === "number" && Number.isInteger(value);
}

function requireInteger(value, name, min, max) {
  if (!isInteger(value)) {
    throw new TypeError("quilt: " + name + " must be an integer");
  }

  if (value < min || value > max) {
    throw new RangeError(
      "quilt: " + name + " must be between " + min + " and " + max
    );
  }

  return value;
}

// validate {width, height, data}, data is row-major RGBA
function validateSource(source) {
  if (source === null || typeof source !== "object") {
    throw new TypeError("quilt: source must be an object");
  }

  const width = source.width;
  const height = source.height;
  const data = source.data;

  if (!isInteger(width) || !isInteger(height)) {
    throw new TypeError("quilt: source width and height must be integers");
  }

  if (width < 2 || width > 64 || height < 2 || height > 64) {
    throw new RangeError(
      "quilt: source width and height must be between 2 and 64"
    );
  }

  if (data === null || typeof data !== "object" || !isInteger(data.length)) {
    throw new TypeError("quilt: source data must be an array or typed array");
  }

  if (data.length !== width * height * 4) {
    throw new RangeError(
      "quilt: source data length must be width * height * 4"
    );
  }

  for (let i = 0; i < data.length; i++) {
    const value = data[i];

    if (!isInteger(value)) {
      throw new TypeError("quilt: source data must contain integers");
    }

    if (value < 0 || value > 255) {
      throw new RangeError("quilt: source data must be between 0 and 255");
    }
  }

  return { width: width, height: height, data: data };
}

// validate the synthesize options
function validateOptions(source, options) {
  if (options === null || typeof options !== "object") {
    throw new TypeError("quilt: options must be an object");
  }

  const width = requireInteger(options.width, "options.width", 6, 128);
  const height = requireInteger(options.height, "options.height", 6, 128);
  const patchWidth = requireInteger(
    options.patchWidth,
    "options.patchWidth",
    3,
    source.width
  );
  const patchHeight = requireInteger(
    options.patchHeight,
    "options.patchHeight",
    3,
    source.height
  );

  if (patchWidth >= width) {
    throw new RangeError("quilt: patchWidth must be smaller than width");
  }

  if (patchHeight >= height) {
    throw new RangeError("quilt: patchHeight must be smaller than height");
  }

  const overlapX = requireInteger(
    options.overlapX,
    "options.overlapX",
    1,
    patchWidth - 1
  );
  const overlapY = requireInteger(
    options.overlapY,
    "options.overlapY",
    1,
    patchHeight - 1
  );

  const tolerance = options.tolerance;

  if (typeof tolerance !== "number" || !Number.isFinite(tolerance)) {
    throw new TypeError("quilt: options.tolerance must be a finite number");
  }

  if (tolerance < 0) {
    throw new RangeError("quilt: options.tolerance must not be negative");
  }

  const seed = options.seed;

  if (!isInteger(seed)) {
    throw new TypeError("quilt: options.seed must be an integer");
  }

  if (seed < 0 || seed >= UINT32) {
    throw new RangeError("quilt: options.seed must be a uint32");
  }

  if (typeof options.wrap !== "boolean") {
    throw new TypeError("quilt: options.wrap must be a boolean");
  }

  return {
    width: width,
    height: height,
    patchWidth: patchWidth,
    patchHeight: patchHeight,
    overlapX: overlapX,
    overlapY: overlapY,
    tolerance: tolerance,
    seed: seed,
    wrap: options.wrap,
  };
}

// local LCG, one step per placed patch
function createLcg(seed) {
  let state = seed >>> 0;

  return function () {
    // 1664525 * state stays below 2^53, so this is exact in a double
    state = (1664525 * state + 1013904223) % UINT32;
    return state;
  };
}

function poolIndex(state, poolSize) {
  // state < 2^32 and poolSize <= 4096
  return Math.floor((state / UINT32) * poolSize);
}

/**
 * Global binary minimum cut.
 *
 * problem = {width, height, oldRGBA, newRGBA, filled, forceOld, forceNew}
 *
 * Every cell carries a label: 0 keeps the old pixel, 1 takes the new pixel.
 * Unfilled cells are forced to 1. Four-neighbour edges pay d[p] + d[q] when
 * the labels of p and q differ, where d is the squared RGB difference of the
 * old and the new pixel of a filled cell and 0 for an unfilled cell.
 */
function cut(problem) {
  if (problem === null || typeof problem !== "object") {
    throw new TypeError("cut: problem must be an object");
  }

  const width = problem.width;
  const height = problem.height;

  if (!isInteger(width) || !isInteger(height)) {
    throw new TypeError("cut: width and height must be integers");
  }

  if (width < 1 || height < 1) {
    throw new RangeError("cut: width and height must be positive");
  }

  const count = width * height;

  const checkRGBA = function (data, name) {
    if (data === null || typeof data !== "object" || !isInteger(data.length)) {
      throw new TypeError("cut: " + name + " must be an array");
    }

    if (data.length < count * 4) {
      throw new RangeError("cut: " + name + " is too short");
    }
  };

  const checkMask = function (mask, name) {
    if (mask === undefined || mask === null) {
      return null;
    }

    if (typeof mask !== "object" || !isInteger(mask.length)) {
      throw new TypeError("cut: " + name + " must be an array");
    }

    if (mask.length !== count) {
      throw new RangeError("cut: " + name + " length must match the problem");
    }

    return mask;
  };

  checkRGBA(problem.oldRGBA, "oldRGBA");
  checkRGBA(problem.newRGBA, "newRGBA");

  const filled = problem.filled;

  if (
    filled === null ||
    typeof filled !== "object" ||
    !isInteger(filled.length)
  ) {
    throw new TypeError("cut: filled must be an array");
  }

  if (filled.length !== count) {
    throw new RangeError("cut: filled length must match the problem");
  }

  const forceOld = checkMask(problem.forceOld, "forceOld");
  const forceNew = checkMask(problem.forceNew, "forceNew");

  // terminal labels: 0 = old, 1 = new, -1 = free
  const terminal = new Int8Array(count);

  for (let p = 0; p < count; p++) {
    const wantOld = forceOld !== null && Boolean(forceOld[p]);
    const wantNew = forceNew !== null && Boolean(forceNew[p]);

    // the two force masks may not prescribe opposite terminals for one cell
    if (wantOld && wantNew) {
      throw new RangeError("cut: forceOld and forceNew conflict");
    }

    if (!filled[p]) {
      // unfilled cells are always forced to the new pixel
      terminal[p] = 1;
    } else if (wantOld) {
      terminal[p] = 0;
    } else if (wantNew) {
      terminal[p] = 1;
    } else {
      terminal[p] = -1;
    }
  }

  // squared RGB difference, alpha is ignored, unfilled cells cost nothing
  const diff = new Float64Array(count);
  const oldRGBA = problem.oldRGBA;
  const newRGBA = problem.newRGBA;

  for (let p = 0; p < count; p++) {
    if (!filled[p]) {
      continue;
    }

    const offset = p * 4;
    const dr = oldRGBA[offset] - newRGBA[offset];
    const dg = oldRGBA[offset + 1] - newRGBA[offset + 1];
    const db = oldRGBA[offset + 2] - newRGBA[offset + 2];

    diff[p] = dr * dr + dg * dg + db * db;
  }

  return solveCut(width, height, diff, terminal);
}

/**
 * Exact minimum cut of a rectangular grid through a maximum flow.
 *
 * Nodes are the cells plus a source and a sink. The source side of the cut is
 * label 0 (keep old), the sink side is label 1 (take new). A four-neighbour
 * pair is an undirected edge of capacity d[p] + d[q]: it is paid exactly when
 * the two cells get different labels.
 *
 * Forced cells become terminal edges: an old-forced cell is tied to the source
 * (source -> p, infinite) and a new-forced cell to the sink (p -> sink,
 * infinite). The infinite capacity is one more than the sum of every finite
 * edge, so an optimal cut never severs a terminal and every terminal is
 * honoured.
 */
function solveCut(width, height, diff, terminal) {
  const count = width * height;

  let pairCapacity = 0;

  for (let y = 0; y < height; y++) {
    for (let x = 0; x < width; x++) {
      const p = y * width + x;

      if (x + 1 < width) {
        pairCapacity += diff[p] + diff[p + 1];
      }

      if (y + 1 < height) {
        pairCapacity += diff[p] + diff[p + width];
      }
    }
  }

  const infinite = pairCapacity + 1;
  const nodes = count + 2;
  const source = count;
  const sink = count + 1;

  const from = [];
  const to = [];
  const cap = [];

  const addEdge = function (u, v, value) {
    if (!(value > 0)) {
      return;
    }

    from.push(u);
    to.push(v);
    cap.push(value);
  };

  for (let y = 0; y < height; y++) {
    for (let x = 0; x < width; x++) {
      const p = y * width + x;

      if (x + 1 < width) {
        const value = diff[p] + diff[p + 1];
        addEdge(p, p + 1, value);
        addEdge(p + 1, p, value);
      }

      if (y + 1 < height) {
        const value = diff[p] + diff[p + width];
        addEdge(p, p + width, value);
        addEdge(p + width, p, value);
      }
    }
  }

  for (let p = 0; p < count; p++) {
    if (terminal[p] === 0) {
      addEdge(source, p, infinite);
    } else if (terminal[p] === 1) {
      addEdge(p, sink, infinite);
    }
  }

  // ---- Dinic ----------------------------------------------------------------
  const edgeCount = from.length * 2;
  const head = new Int32Array(nodes);
  head.fill(-1);

  const edgeTo = new Int32Array(edgeCount);
  const edgeCap = new Float64Array(edgeCount);
  const edgeNext = new Int32Array(edgeCount);

  let edgeIndex = 0;

  const addPair = function (u, v, value) {
    edgeTo[edgeIndex] = v;
    edgeCap[edgeIndex] = value;
    edgeNext[edgeIndex] = head[u];
    head[u] = edgeIndex;
    edgeIndex++;

    edgeTo[edgeIndex] = u;
    edgeCap[edgeIndex] = 0;
    edgeNext[edgeIndex] = head[v];
    head[v] = edgeIndex;
    edgeIndex++;
  };

  for (let i = 0; i < from.length; i++) {
    addPair(from[i], to[i], cap[i]);
  }

  const level = new Int32Array(nodes);
  const iter = new Int32Array(nodes);
  const queue = new Int32Array(nodes);

  const bfs = function () {
    level.fill(-1);
    level[source] = 0;
    let qh = 0;
    let qt = 0;
    queue[qt++] = source;

    while (qh < qt) {
      const u = queue[qh++];

      for (let e = head[u]; e !== -1; e = edgeNext[e]) {
        if (edgeCap[e] > 0 && level[edgeTo[e]] < 0) {
          level[edgeTo[e]] = level[u] + 1;
          queue[qt++] = edgeTo[e];
        }
      }
    }

    return level[sink] >= 0;
  };

  const dfs = function (u, available) {
    if (u === sink) {
      return available;
    }

    for (; iter[u] !== -1; iter[u] = edgeNext[iter[u]]) {
      const e = iter[u];

      if (edgeCap[e] <= 0 || level[edgeTo[e]] !== level[u] + 1) {
        continue;
      }

      const pushed = dfs(edgeTo[e], Math.min(available, edgeCap[e]));

      if (pushed > 0) {
        edgeCap[e] -= pushed;
        edgeCap[e ^ 1] += pushed;
        return pushed;
      }
    }

    return 0;
  };

  while (bfs()) {
    for (let i = 0; i < nodes; i++) {
      iter[i] = head[i];
    }

    let pushed;

    do {
      pushed = dfs(source, Number.MAX_VALUE);
    } while (pushed > 0);
  }

  // reachable from the source in the residual graph = the old (label 0) side
  const reachable = new Uint8Array(nodes);
  reachable[source] = 1;
  queue[0] = source;
  let qh = 0;
  let qt = 1;

  while (qh < qt) {
    const u = queue[qh++];

    for (let e = head[u]; e !== -1; e = edgeNext[e]) {
      if (edgeCap[e] > 0 && !reachable[edgeTo[e]]) {
        reachable[edgeTo[e]] = 1;
        queue[qt++] = edgeTo[e];
      }
    }
  }

  const labels = new Uint8Array(count);

  for (let p = 0; p < count; p++) {
    labels[p] = reachable[p] ? 0 : 1;
  }

  // terminal labels always win, even for a component cut off from the flow
  for (let p = 0; p < count; p++) {
    if (terminal[p] === 0) {
      labels[p] = 0;
    } else if (terminal[p] === 1) {
      labels[p] = 1;
    }
  }

  // the energy is evaluated on the labels, not on the flow
  let energy = 0;

  for (let y = 0; y < height; y++) {
    for (let x = 0; x < width; x++) {
      const p = y * width + x;

      if (x + 1 < width && labels[p] !== labels[p + 1]) {
        energy += diff[p] + diff[p + 1];
      }

      if (y + 1 < height && labels[p] !== labels[p + width]) {
        energy += diff[p] + diff[p + width];
      }
    }
  }

  return { labels: labels, energy: energy };
}

/**
 * Synthesize a larger texture from a small source image.
 *
 * @param {{width:number,height:number,data:ArrayLike<number>}} source
 * @param {object} options
 * @returns {{width:number,height:number,data:Uint8ClampedArray,
 *   provenance:Int32Array,placements:Array}}
 */
function synthesize(source, options) {
  const src = validateSource(source);
  const opt = validateOptions(src, options);

  const srcWidth = src.width;
  const srcData = src.data;

  const width = opt.width;
  const height = opt.height;
  const patchWidth = opt.patchWidth;
  const patchHeight = opt.patchHeight;
  const overlapX = opt.overlapX;
  const overlapY = opt.overlapY;
  const wrap = opt.wrap;
  const outputCount = width * height;

  const mask = new Uint8Array(outputCount);
  const provenance = new Int32Array(outputCount);
  provenance.fill(-1);

  const outData = new Uint8ClampedArray(outputCount * 4);
  const placements = [];

  // candidate positions in the source, ordered by source y then x
  const candidateCountX = srcWidth - patchWidth + 1;
  const candidateCountY = src.height - patchHeight + 1;
  const candidateCount = candidateCountX * candidateCountY;

  const candidateX = new Int32Array(candidateCount);
  const candidateY = new Int32Array(candidateCount);
  let candidateIndex = 0;

  for (let sy = 0; sy < candidateCountY; sy++) {
    for (let sx = 0; sx < candidateCountX; sx++) {
      candidateX[candidateIndex] = sx;
      candidateY[candidateIndex] = sy;
      candidateIndex++;
    }
  }

  const context = {
    width: width,
    height: height,
    patchWidth: patchWidth,
    patchHeight: patchHeight,
    wrap: wrap,
    tolerance: opt.tolerance,
    srcWidth: srcWidth,
    srcData: srcData,
    candidateCount: candidateCount,
    candidateX: candidateX,
    candidateY: candidateY,
    outData: outData,
    mask: mask,
    provenance: provenance,
  };

  const lcgNext = createLcg(opt.seed);

  for (let py = 0; py < height; py += patchHeight - overlapY) {
    for (let px = 0; px < width; px += patchWidth - overlapX) {
      placements.push(placePatch(px, py, context, lcgNext));
    }
  }

  return {
    width: width,
    height: height,
    data: outData,
    provenance: provenance,
    placements: placements,
  };
}

// score a candidate patch against the real filled pixels of the output
function scoreCandidate(context, x, y, sourceX, sourceY) {
  const width = context.width;
  const height = context.height;
  const srcWidth = context.srcWidth;
  const srcData = context.srcData;
  const outData = context.outData;
  const mask = context.mask;
  const wrap = context.wrap;

  const effectiveWidth = wrap ? context.patchWidth : Math.min(context.patchWidth, width - x);
  const effectiveHeight = wrap ? context.patchHeight : Math.min(context.patchHeight, height - y);

  let score = 0;

  for (let by = 0; by < effectiveHeight; by++) {
    let oy = y + by;

    if (wrap) {
      oy = ((oy % height) + height) % height;
    } else if (oy < 0 || oy >= height) {
      continue;
    }

    for (let bx = 0; bx < effectiveWidth; bx++) {
      let ox = x + bx;

      if (wrap) {
        ox = ((ox % width) + width) % width;
      } else if (ox < 0 || ox >= width) {
        continue;
      }

      const o = position(ox, oy, width);

      if (!mask[o]) {
        continue;
      }

      const s = position(sourceX + bx, sourceY + by, srcWidth) * 4;
      const d = o * 4;
      const dr = outData[d] - srcData[s];
      const dg = outData[d + 1] - srcData[s + 1];
      const db = outData[d + 2] - srcData[s + 2];

      score += dr * dr + dg * dg + db * db;
    }
  }

  return score;
}

// is an output cell covered by the block that starts at x, y?
function blockCovers(context, x, y, ox, oy) {
  let dx = ox - x;
  let dy = oy - y;

  if (context.wrap) {
    dx = ((dx % context.width) + context.width) % context.width;
    dy = ((dy % context.height) + context.height) % context.height;

    // the block is smaller than the output on both axes, so the mapping is
    // unique and a cell is covered exactly when it lies inside the block
    return dx < context.patchWidth && dy < context.patchHeight;
  }

  return dx >= 0 && dx < context.patchWidth && dy >= 0 && dy < context.patchHeight;
}

function placePatch(x, y, context, lcgNext) {
  const width = context.width;
  const height = context.height;
  const patchWidth = context.patchWidth;
  const patchHeight = context.patchHeight;
  const wrap = context.wrap;
  const tolerance = context.tolerance;
  const srcData = context.srcData;
  const srcWidth = context.srcWidth;
  const candidateCount = context.candidateCount;
  const candidateX = context.candidateX;
  const candidateY = context.candidateY;
  const outData = context.outData;
  const mask = context.mask;
  const provenance = context.provenance;

  const effectiveWidth = wrap ? patchWidth : Math.min(patchWidth, width - x);
  const effectiveHeight = wrap ? patchHeight : Math.min(patchHeight, height - y);

  // score every full patch position of the source
  const scores = new Float64Array(candidateCount);
  let minError = Infinity;

  for (let i = 0; i < candidateCount; i++) {
    const value = scoreCandidate(
      context,
      x,
      y,
      candidateX[i],
      candidateY[i]
    );

    scores[i] = value;

    if (value < minError) {
      minError = value;
    }
  }

  // pool of candidates within tolerance of the best one
  const pool = [];

  if (minError === 0) {
    for (let i = 0; i < candidateCount; i++) {
      if (scores[i] === 0) {
        pool.push(i);
      }
    }
  } else {
    const limit = minError * (1 + tolerance);

    for (let i = 0; i < candidateCount; i++) {
      if (scores[i] <= limit) {
        pool.push(i);
      }
    }
  }

  // exactly one local LCG step per placement
  const state = lcgNext();
  const chosen = pool[poolIndex(state, pool.length)];
  const sourceX = candidateX[chosen];
  const sourceY = candidateY[chosen];
  const score = scores[chosen];

  // build the local cut problem
  const localWidth = effectiveWidth;
  const localHeight = effectiveHeight;
  const localCount = localWidth * localHeight;

  const oldRGBA = new Uint8ClampedArray(localCount * 4);
  const newRGBA = new Uint8ClampedArray(localCount * 4);
  const filled = new Uint8Array(localCount);
  const forceOld = new Uint8Array(localCount);
  const forceNew = new Uint8Array(localCount);

  // output index of every local cell, -1 when the cell is clipped away
  const localOutput = new Int32Array(localCount);
  localOutput.fill(-1);

  for (let ly = 0; ly < localHeight; ly++) {
    for (let lx = 0; lx < localWidth; lx++) {
      const l = position(lx, ly, localWidth);
      let ox = x + lx;
      let oy = y + ly;

      if (wrap) {
        ox = ((ox % width) + width) % width;
        oy = ((oy % height) + height) % height;
      } else if (ox < 0 || ox >= width || oy < 0 || oy >= height) {
        continue;
      }

      const o = position(ox, oy, width);
      localOutput[l] = o;

      const s = position(sourceX + lx, sourceY + ly, srcWidth) * 4;

      newRGBA[l * 4] = srcData[s];
      newRGBA[l * 4 + 1] = srcData[s + 1];
      newRGBA[l * 4 + 2] = srcData[s + 2];
      newRGBA[l * 4 + 3] = srcData[s + 3];

      if (mask[o]) {
        filled[l] = 1;

        oldRGBA[l * 4] = outData[o * 4];
        oldRGBA[l * 4 + 1] = outData[o * 4 + 1];
        oldRGBA[l * 4 + 2] = outData[o * 4 + 2];
        oldRGBA[l * 4 + 3] = outData[o * 4 + 3];
      } else {
        // unfilled cells always take the new pixel
        forceNew[l] = 1;
      }
    }
  }

  // a filled cell with an already filled four-neighbour outside this block
  // keeps the old pixel (the neighbour is wrapped in wrap mode)
  for (let ly = 0; ly < localHeight; ly++) {
    for (let lx = 0; lx < localWidth; lx++) {
      const l = position(lx, ly, localWidth);

      if (!filled[l]) {
        continue;
      }

      let ox = x + lx;
      let oy = y + ly;

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

      for (let i = 0; i < 4; i++) {
        let nx = neighbours[i][0];
        let ny = neighbours[i][1];

        if (wrap) {
          nx = ((nx % width) + width) % width;
          ny = ((ny % height) + height) % height;
        } else if (nx < 0 || nx >= width || ny < 0 || ny >= height) {
          continue;
        }

        const n = position(nx, ny, width);

        if (mask[n] && !blockCovers(context, x, y, nx, ny)) {
          forceOld[l] = 1;
          break;
        }
      }
    }
  }

  // a patch that covers only filled cells forces the local center to the new
  // pixel; if that cell is also forced to the old pixel the block is skipped
  let allFilled = true;

  for (let l = 0; l < localCount; l++) {
    if (!filled[l]) {
      allFilled = false;
      break;
    }
  }

  if (allFilled) {
    const centerX = Math.floor(localWidth / 2);
    const centerY = Math.floor(localHeight / 2);
    const center = position(centerX, centerY, localWidth);

    if (forceOld[center]) {
      return {
        x: x,
        y: y,
        sourceX: sourceX,
        sourceY: sourceY,
        candidateCount: candidateCount,
        score: score,
        energy: 0,
        skipped: true,
        labels: [],
      };
    }

    forceNew[center] = 1;
  }

  const result = cut({
    width: localWidth,
    height: localHeight,
    oldRGBA: oldRGBA,
    newRGBA: newRGBA,
    filled: filled,
    forceOld: forceOld,
    forceNew: forceNew,
  });

  // write the pixels chosen by the cut
  for (let l = 0; l < localCount; l++) {
    const o = localOutput[l];

    if (o === -1) {
      continue;
    }

    if (result.labels[l] === 1) {
      outData[o * 4] = newRGBA[l * 4];
      outData[o * 4 + 1] = newRGBA[l * 4 + 1];
      outData[o * 4 + 2] = newRGBA[l * 4 + 2];
      outData[o * 4 + 3] = newRGBA[l * 4 + 3];
      provenance[o] = sourceY + Math.floor(l / localWidth);
    }

    mask[o] = 1;
  }

  return {
    x: x,
    y: y,
    sourceX: sourceX,
    sourceY: sourceY,
    candidateCount: candidateCount,
    score: score,
    energy: result.energy,
    skipped: false,
    labels: Array.from(result.labels),
  };
}

// tgen effect, output size comes from the generator itself
function register(tgen) {
  const effect = function ($g, params) {
    if (params.source === null || typeof params.source !== "object") {
      throw new TypeError("quilt: params.source is required");
    }

    if (params.options === null || typeof params.options !== "object") {
      throw new TypeError("quilt: params.options is required");
    }

    const input = params.options;

    const options = {
      width: $g.texture.width,
      height: $g.texture.height,
      patchWidth: input.patchWidth,
      patchHeight: input.patchHeight,
      overlapX: input.overlapX,
      overlapY: input.overlapY,
      tolerance: input.tolerance,
      seed: input.seed,
      wrap: input.wrap,
    };

    const result = synthesize(params.source, options);
    const data = $g.texture.data;

    for (let i = 0; i < result.data.length; i++) {
      data[i] = result.data[i];
    }

    // stored params stay JSON friendly: plain source array, plain options
    params.source = {
      width: params.source.width,
      height: params.source.height,
      data: Array.from(params.source.data),
    };
    params.options = options;

    return params;
  };

  // the quilt uses its own LCG, the generator random sequence is untouched
  effect.preserveSeed = true;

  tgen.effect(
    "quilt",
    {
      source: null,
      options: null,
    },
    effect
  );

  tgen.quilt = synthesize;
}

module.exports = {
  cut: cut,
  synthesize: synthesize,
  register: register,
};
