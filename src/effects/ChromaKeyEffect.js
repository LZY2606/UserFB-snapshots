/**
 * @module ChromaKeyEffect
 * Removes a solid background colour (green screen, blue screen, or any other
 * flat backdrop) from a video clip, making matching pixels transparent.
 */

import Effect from './Effect.js';
import { EFFECT_TYPES } from '../utils/Constants.js';

class ChromaKeyEffect extends Effect {
  /**
   * @param {object} [params={}]
   * @param {string}  [params.color='#00FF00']  - Background colour to remove (hex or CSS colour name).
   * @param {number}  [params.similarity=0.10]  - How close a pixel's colour must be to `color` to be
   *   keyed out, in [0, 1]. Higher values key out a wider range of shades.
   * @param {number}  [params.blend=0.10]       - Softness of the key edge, in [0, 1]. Higher values
   *   produce a smoother transition between opaque and transparent pixels.
   * @param {boolean} [params.yuv=false]        - Match colour similarity in YUV space instead of RGB.
   *   Generally gives cleaner edges on compressed (YUV-encoded) source footage.
   */
  constructor(params = {}) {
    super(EFFECT_TYPES.CHROMA_KEY, {
      color:      params.color      ?? '#00FF00',
      similarity: params.similarity ?? 0.10,
      blend:      params.blend      ?? 0.10,
      yuv:        params.yuv        ?? false,
    });
  }

  // ─── Accessors ───────────────────────────────────────────────────────────────

  get color()      { return this.params.color; }
  set color(v)     { this.params.color = v; }

  get similarity() { return this.params.similarity; }
  set similarity(v) {
    if (v < 0 || v > 1) throw new RangeError('ChromaKeyEffect similarity must be in [0, 1]');
    this.params.similarity = v;
  }

  get blend()      { return this.params.blend; }
  set blend(v) {
    if (v < 0 || v > 1) throw new RangeError('ChromaKeyEffect blend must be in [0, 1]');
    this.params.blend = v;
  }

  get yuv()        { return this.params.yuv; }
  set yuv(v)       { this.params.yuv = Boolean(v); }

  // ─── Apply ───────────────────────────────────────────────────────────────────

  apply(context) {
    // TODO: Wire into the renderer's compositing pipeline.
    if (!this.enabled) return context;
    return { ...context, chromaKey: { ...this.params } };
  }

  // ─── Serialisation ───────────────────────────────────────────────────────────

  toJSON() {
    return super.toJSON();
  }

  /**
   * @param {object} data
   * @returns {ChromaKeyEffect}
   */
  static fromJSON(data) {
    const fx = new ChromaKeyEffect(data.params ?? {});
    fx.id        = data.id;
    fx.enabled   = data.enabled;
    fx.createdAt = new Date(data.createdAt);
    return fx;
  }
}

export default ChromaKeyEffect;
