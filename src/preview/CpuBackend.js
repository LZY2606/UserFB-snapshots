/**
 * @module CpuBackend
 * A pure-JavaScript rendering backend for PreviewRenderer.
 *
 * CpuBackend composites the timeline into an in-memory RGBA buffer using
 * nothing but typed arrays — no DOM, no canvas, no native dependencies.
 * Decoded video frames are supplied by the caller through `readFrame`,
 * which may return synchronously or via a Promise.
 *
 * Frame contract: `{ width, height, data }` where `data` is a
 * Uint8ClampedArray of non-premultiplied RGBA bytes, row by row.
 *
 * Guarantees:
 *   - Every render starts from a fully transparent canvas (`clear`).
 *   - Frames returned by getFrame() are fresh copies — later renders never
 *     mutate or reuse previously returned storage.
 *   - Source frames delivered by `readFrame` are only ever read.
 */

import { sourceOverInto } from '../utils/FrameOps.js';

class CpuBackend {
  /**
   * @param {object} options
   * @param {number} options.width  - Output width in pixels.
   * @param {number} options.height - Output height in pixels.
   * @param {function(object, number): (object|Promise<object>)} options.readFrame
   *        Called as readFrame(clip, sourceTime) and must yield
   *        `{ width, height, data }` (non-premultiplied RGBA bytes).
   */
  constructor({ width, height, readFrame } = {}) {
    if (!Number.isFinite(width) || width <= 0 || !Number.isFinite(height) || height <= 0) {
      throw new RangeError('CpuBackend requires positive integer width and height.');
    }
    if (typeof readFrame !== 'function') {
      throw new TypeError('CpuBackend requires a readFrame(clip, sourceTime) function.');
    }

    /** @type {number} */
    this.width = width;

    /** @type {number} */
    this.height = height;

    /** @type {function(object, number): (object|Promise<object>)} */
    this.readFrame = readFrame;

    /**
     * Internal compositing buffer.  Never handed out — getFrame() copies.
     * @type {Uint8ClampedArray}
     */
    this._buffer = new Uint8ClampedArray(width * height * 4);
  }

  // ─── PreviewRenderer backend interface ─────────────────────────────────────

  /** Reset the canvas to fully transparent. */
  clear() {
    this._buffer.fill(0);
  }

  /**
   * Draw a video clip sampled at `sourceTime`, composited source-over.
   * @param {import('../clips/VideoClip.js').default} clip
   * @param {number} sourceTime - Source-media time in seconds.
   * @returns {Promise<void>}
   */
  async drawVideo(clip, sourceTime) {
    const frame = await this.readFrame(clip, sourceTime);
    if (frame == null) return;
    this._assertFrame(frame);
    sourceOverInto(this._destination(), frame);
  }

  /**
   * Draw a transition layer: sample both clips, run the Transition's own
   * apply() to blend them into a single frame, then composite it.
   * @param {object} layer
   * @param {import('../effects/Transition.js').default} layer.transition
   * @param {import('../clips/VideoClip.js').default} layer.fromClip
   * @param {number} layer.fromSourceTime
   * @param {import('../clips/VideoClip.js').default} layer.toClip
   * @param {number} layer.toSourceTime
   * @param {number} layer.elapsed - Seconds elapsed inside the transition.
   * @returns {Promise<void>}
   */
  async drawTransition(layer) {
    const { transition, fromClip, fromSourceTime, toClip, toSourceTime, elapsed } = layer;
    const [fromFrame, toFrame] = await Promise.all([
      this.readFrame(fromClip, fromSourceTime),
      this.readFrame(toClip, toSourceTime),
    ]);
    this._assertFrame(fromFrame);
    this._assertFrame(toFrame);
    const context = transition.apply({
      time: elapsed,
      fromFrame,
      toFrame,
      width: this.width,
      height: this.height,
    });
    if (!context || context.frame == null) return;
    this._assertFrame(context.frame);
    sourceOverInto(this._destination(), context.frame);
  }

  /**
   * Return the composited frame.  The returned object owns fresh storage
   * on every call.
   * @returns {{ width: number, height: number, data: Uint8ClampedArray }}
   */
  getFrame() {
    return {
      width: this.width,
      height: this.height,
      data: new Uint8ClampedArray(this._buffer),
    };
  }

  // ─── Internal helpers ───────────────────────────────────────────────────────

  _destination() {
    return { width: this.width, height: this.height, data: this._buffer };
  }

  _assertFrame(frame) {
    const ok = frame
      && Number.isFinite(frame.width) && frame.width > 0
      && Number.isFinite(frame.height) && frame.height > 0
      && frame.data
      && frame.data.length >= frame.width * frame.height * 4;
    if (!ok) {
      throw new Error(
        'CpuBackend: readFrame must return { width, height, data } with ' +
        'data.length >= width * height * 4.',
      );
    }
  }
}

export default CpuBackend;
