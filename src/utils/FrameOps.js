/**
 * @module FrameOps
 * Pure CPU pixel operations on RGBA frames.
 *
 * A frame is a plain object `{ width, height, data }` where `data` is a
 * Uint8ClampedArray of non-premultiplied RGBA bytes in row-major order.
 * None of these functions mutate their source frames; operations that
 * composite into a destination are named `*Into` and say so explicitly.
 */

/**
 * Allocate a fully transparent frame.
 * @param {number} width
 * @param {number} height
 * @returns {{ width: number, height: number, data: Uint8ClampedArray }}
 */
export function createFrame(width, height) {
  return { width, height, data: new Uint8ClampedArray(width * height * 4) };
}

/**
 * Composite `src` over `dst` (source-over) in place.  `src` is only read.
 * Both frames hold non-premultiplied RGBA bytes; the result is
 * non-premultiplied, rounded per channel to the nearest byte.  Fully
 * transparent destination pixels keep RGB = 0.
 *
 * @param {{ width: number, height: number, data: Uint8ClampedArray }} dst - Mutated.
 * @param {{ width: number, height: number, data: Uint8ClampedArray }} src - Read only.
 * @returns {object} dst
 */
export function sourceOverInto(dst, src) {
  const w = Math.min(dst.width, src.width);
  const h = Math.min(dst.height, src.height);
  for (let y = 0; y < h; y++) {
    for (let x = 0; x < w; x++) {
      const di = (y * dst.width + x) * 4;
      const si = (y * src.width + x) * 4;
      const sa = src.data[si + 3] / 255;
      if (sa <= 0) continue;
      const da = dst.data[di + 3] / 255;
      const oa = sa + da * (1 - sa);
      for (let c = 0; c < 3; c++) {
        dst.data[di + c] = Math.round(
          (src.data[si + c] * sa + dst.data[di + c] * da * (1 - sa)) / oa,
        );
      }
      dst.data[di + 3] = Math.round(oa * 255);
    }
  }
  return dst;
}

/**
 * Cross-dissolve two frames: interpolate premultiplied colour and alpha
 * by `progress`, then convert back to non-premultiplied bytes.
 * Fully transparent results keep RGB = 0.
 *
 * @param {{ width: number, height: number, data: Uint8ClampedArray }} fromFrame
 * @param {{ width: number, height: number, data: Uint8ClampedArray }} toFrame
 * @param {number} progress - 0 = fully `fromFrame`, 1 = fully `toFrame`.
 * @returns {{ width: number, height: number, data: Uint8ClampedArray }} New frame.
 */
export function blendFrames(fromFrame, toFrame, progress) {
  const p = Math.min(Math.max(progress, 0), 1);
  const width = Math.min(fromFrame.width, toFrame.width);
  const height = Math.min(fromFrame.height, toFrame.height);
  const out = createFrame(width, height);
  for (let y = 0; y < height; y++) {
    for (let x = 0; x < width; x++) {
      const fi = (y * fromFrame.width + x) * 4;
      const ti = (y * toFrame.width + x) * 4;
      const oi = (y * width + x) * 4;
      const fa = fromFrame.data[fi + 3] / 255;
      const ta = toFrame.data[ti + 3] / 255;
      const oa = fa * (1 - p) + ta * p;
      if (oa <= 0) continue; // fully transparent — RGB stays 0
      for (let c = 0; c < 3; c++) {
        const premult = fromFrame.data[fi + c] * fa * (1 - p) + toFrame.data[ti + c] * ta * p;
        out.data[oi + c] = Math.round(premult / oa);
      }
      out.data[oi + 3] = Math.round(oa * 255);
    }
  }
  return out;
}

/**
 * Left-to-right wipe: columns where `x / width < progress` reveal
 * `toFrame` (the incoming clip); everything else shows `fromFrame`.
 *
 * @param {{ width: number, height: number, data: Uint8ClampedArray }} fromFrame
 * @param {{ width: number, height: number, data: Uint8ClampedArray }} toFrame
 * @param {number} progress - 0–1.
 * @returns {{ width: number, height: number, data: Uint8ClampedArray }} New frame.
 */
export function wipeLeftFrames(fromFrame, toFrame, progress) {
  const p = Math.min(Math.max(progress, 0), 1);
  const width = Math.min(fromFrame.width, toFrame.width);
  const height = Math.min(fromFrame.height, toFrame.height);
  const out = createFrame(width, height);
  for (let y = 0; y < height; y++) {
    for (let x = 0; x < width; x++) {
      const showIncoming = x / width < p;
      const src = showIncoming ? toFrame : fromFrame;
      const si = (y * src.width + x) * 4;
      const oi = (y * width + x) * 4;
      out.data[oi] = src.data[si];
      out.data[oi + 1] = src.data[si + 1];
      out.data[oi + 2] = src.data[si + 2];
      out.data[oi + 3] = src.data[si + 3];
    }
  }
  return out;
}
