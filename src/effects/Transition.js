/**
 * @module Transition
 * Represents a transition between two adjacent Clips on a Track.
 *
 * A Transition is an Effect with an overlap region: the final `duration`
 * seconds of `fromClip` overlap with the first `duration` seconds of `toClip`.
 */

import Effect from './Effect.js';
import { EFFECT_TYPES, TRANSITION_TYPES, EASING } from '../utils/Constants.js';
import { blendFrames, wipeLeftFrames } from '../utils/FrameOps.js';

class Transition extends Effect {
  /**
   * @param {string} [transitionType=TRANSITION_TYPES.CROSS_DISSOLVE]
   * @param {number} [duration=1] - Overlap duration in seconds.
   * @param {object} [options={}]
   * @param {string} [options.easing=EASING.EASE_IN_OUT]
   * @param {string|null} [options.fromClipId=null] - ID of the outgoing clip.
   * @param {string|null} [options.toClipId=null]   - ID of the incoming clip.
   */
  constructor(
    transitionType = TRANSITION_TYPES.CROSS_DISSOLVE,
    duration = 1,
    options = {},
  ) {
    super(EFFECT_TYPES.TRANSITION, {
      transitionType,
      duration,
      easing: options.easing ?? EASING.EASE_IN_OUT,
      fromClipId: options.fromClipId ?? null,
      toClipId: options.toClipId ?? null,
    });

    /** @type {string} */
    this.transitionType = transitionType;
  }

  // ─── Accessors ───────────────────────────────────────────────────────────────

  /** @returns {number} */
  get duration() {
    return this.params.duration;
  }

  /** @param {number} value */
  set duration(value) {
    if (value <= 0) throw new RangeError('Transition duration must be > 0');
    this.params.duration = value;
  }

  /**
   * Bind the two clips this transition connects.
   *
   * @param {import('../core/Clip.js').default} fromClip
   * @param {import('../core/Clip.js').default} toClip
   * @returns {Transition} this (chainable)
   */
  link(fromClip, toClip) {
    this.params.fromClipId = fromClip.id;
    this.params.toClipId = toClip.id;
    return this;
  }

  /**
   * Compute the blend progress at a given time within the overlap window.
   *
   * @param {number} t - Seconds into the transition (0 = start, duration = end).
   * @returns {number} Progress in [0, 1].
   */
  getProgressAt(t) {
    const raw = Math.min(Math.max(t / this.params.duration, 0), 1);
    return Transition._ease(raw, this.params.easing);
  }

  /**
   * Apply the transition within a composite render context.
   *
   * Composites `context.fromFrame` and `context.toFrame` into a new frame
   * written to `context.frame` of the returned context.  `context.time` is
   * the number of seconds already elapsed inside the transition window.
   * Frames are `{ width, height, data }` non-premultiplied RGBA byte
   * buffers; the input frames are never mutated.
   *
   * Supported types: `crossDissolve`, `wipeLeft`.  Any other enabled type
   * throws; a disabled transition is a no-op.
   *
   * @param {object} context - { time, fromFrame, toFrame, ... }
   * @returns {object} The same context, with `frame` populated.
   */
  apply(context) {
    if (!this.enabled) return context;
    const { fromFrame, toFrame } = context ?? {};
    if (!fromFrame || !toFrame) {
      throw new Error('Transition.apply requires context.fromFrame and context.toFrame.');
    }
    const progress = this.getProgressAt(context.time ?? 0);
    switch (this.transitionType) {
      case TRANSITION_TYPES.CROSS_DISSOLVE:
        context.frame = blendFrames(fromFrame, toFrame, progress);
        break;
      case TRANSITION_TYPES.WIPE_LEFT:
        context.frame = wipeLeftFrames(fromFrame, toFrame, progress);
        break;
      default:
        throw new Error(
          `Transition type "${this.transitionType}" is not supported for preview. ` +
          `Supported types: ${TRANSITION_TYPES.CROSS_DISSOLVE}, ${TRANSITION_TYPES.WIPE_LEFT}.`,
        );
    }
    return context;
  }

  // ─── Private helpers ─────────────────────────────────────────────────────────

  static _ease(t, easing) {
    switch (easing) {
      case 'easeIn':    return t * t;
      case 'easeOut':   return t * (2 - t);
      case 'easeInOut': return t < 0.5 ? 2 * t * t : -1 + (4 - 2 * t) * t;
      default:          return t;
    }
  }

  // ─── Serialisation ───────────────────────────────────────────────────────────

  toJSON() {
    return { ...super.toJSON(), transitionType: this.transitionType };
  }

  /**
   * @param {object} data
   * @returns {Transition}
   */
  static fromJSON(data) {
    const t = new Transition(data.transitionType, data.params.duration, {
      easing: data.params.easing,
      fromClipId: data.params.fromClipId,
      toClipId: data.params.toClipId,
    });
    t.id = data.id;
    t.enabled = data.enabled;
    t.createdAt = new Date(data.createdAt);
    return t;
  }
}

export default Transition;
