/**
 * @module Clip
 * Abstract base class for all clip types (video, audio, image, text, shape).
 *
 * A Clip maps a source Asset (or synthetic content) onto a position on the
 * project Timeline.  It carries its own effect chain and exposes the editing
 * verbs (trim, split, move, copy, fade, etc.) that higher-level APIs use.
 */

import IdGenerator from '../utils/IdGenerator.js';
import { CLIP_TYPES } from '../utils/Constants.js';
import FadeEffect from '../effects/FadeEffect.js';
import CropEffect from '../effects/CropEffect.js';
import ChromaKeyEffect from '../effects/ChromaKeyEffect.js';

class Clip {
  /**
   * @param {import('./Asset.js').default|null} asset - Source asset, or null for synthetic clips.
   * @param {object} [options={}]
   * @param {string} [options.type=CLIP_TYPES.VIDEO]
   * @param {number} [options.startTime=0]   - Timeline position in seconds.
   * @param {number} [options.inPoint=0]     - Source in-point in seconds.
   * @param {number} [options.outPoint=null] - Source out-point; defaults to asset.duration.
   * @param {string} [options.name='']
   */
  constructor(asset, options = {}) {
    if (new.target === Clip) {
      throw new TypeError('Clip is abstract — instantiate a concrete subclass.');
    }

    /** @type {string} */
    this.id = IdGenerator.generate('clip');

    /** @type {string} */
    this.type = options.type ?? CLIP_TYPES.VIDEO;

    /** @type {string} */
    this.name = options.name ?? '';

    /** @type {import('./Asset.js').default|null} */
    this.asset = asset;

    /** @type {number} Position on the timeline (seconds). */
    this.startTime = options.startTime ?? 0;

    /** @type {number} Where in the source media this clip starts (seconds). */
    this.inPoint = options.inPoint ?? 0;

    /**
     * Where in the source media this clip ends (seconds).
     * Defaults to the asset's total duration if available, else Infinity.
     * @type {number}
     */
    this.outPoint = options.outPoint ?? (asset?.duration || Infinity);

    /** @type {Effect[]} Ordered effect chain applied during render/export. */
    this.effects = [];

    /** @type {boolean} */
    this.muted = false;

    /** @type {boolean} */
    this.locked = false;

    /** @type {boolean} */
    this.visible = true;

    /** @type {Date} */
    this.createdAt = new Date();

    // Back-reference to the owning Track — set by Track when the clip is added.
    /** @type {import('./Track.js').default|null} */
    this._track = null;
  }

  // ─── Computed properties ─────────────────────────────────────────────────────

  /**
   * Playback duration on the timeline (seconds).
   * @returns {number}
   */
  get duration() {
    return this.outPoint - this.inPoint;
  }

  /**
   * The timeline time at which this clip ends.
   * @returns {number}
   */
  get endTime() {
    return this.startTime + this.duration;
  }

  /**
   * Map a timeline-absolute time to the corresponding source-media time.
   * The base mapping is 1:1 from the in-point; rate-aware clips
   * (VideoClip, AudioClip) override this.
   *
   * @param {number} time - Timeline position in seconds.
   * @returns {number} Source-media time in seconds.
   */
  getSourceTimeAt(time) {
    return this.inPoint + (time - this.startTime);
  }

  // ─── Editing verbs ────────────────────────────────────────────────────────────

  /**
   * Set in- and out-points to restrict which portion of the source plays.
   * Does NOT change the clip's position on the timeline.
   *
   * @param {number} inPoint  - New in-point (seconds, relative to source).
   * @param {number} outPoint - New out-point (seconds, relative to source).
   * @returns {Clip} this (chainable)
   */
  trim(inPoint, outPoint) {
    if (inPoint < 0) throw new RangeError('inPoint must be >= 0');
    if (outPoint <= inPoint) throw new RangeError('outPoint must be > inPoint');
    this.inPoint = inPoint;
    this.outPoint = outPoint;
    return this;
  }

  /**
   * Move the clip to a new position on the timeline.
   *
   * @param {number} startTime - New timeline start position (seconds).
   * @returns {Clip} this (chainable)
   */
  move(startTime) {
    if (startTime < 0) throw new RangeError('startTime must be >= 0');
    this.startTime = startTime;
    return this;
  }

  /**
   * Split this clip at `time` (a timeline-absolute position).
   *
   * The original clip is shortened to end at `time`.
   * A new sibling clip (the "tail") is created from `time` to the original end.
   * Both clips are registered on the owning track (if available).
   *
   * @param {number} time - Absolute timeline time at which to split (seconds).
   * @returns {{ head: Clip, tail: Clip }}
   */
  split(time) {
    if (time <= this.startTime || time >= this.endTime) {
      throw new RangeError(
        `Split time ${time}s is outside clip range [${this.startTime}, ${this.endTime}]`,
      );
    }

    const range = this._splitSourceRange(time);

    // Tail: copy of this clip, re-pointed to the second half of the source.
    const tail = this.copy();
    tail.startTime = time;
    tail.inPoint = range.tailIn;
    tail.outPoint = range.tailOut;

    // Head (this clip): shortened.
    this.inPoint = range.headIn;
    this.outPoint = range.headOut;

    if (this._track) {
      this._track._insertClipAfter(this, tail);
    }

    return { head: this, tail };
  }

  /**
   * Source ranges for the two halves of a split at timeline time `time`.
   * The base mapping is 1:1 from the in-point; rate-aware and reversed
   * clips override this so that sampling stays continuous across the cut.
   *
   * @param {number} time - Absolute timeline time of the split (seconds).
   * @returns {{ headIn: number, headOut: number, tailIn: number, tailOut: number }}
   * @protected
   */
  _splitSourceRange(time) {
    const cut = this.inPoint + (time - this.startTime);
    return { headIn: this.inPoint, headOut: cut, tailIn: cut, tailOut: this.outPoint };
  }

  /**
   * Create a deep copy of this clip with a new ID.
   * Effects are cloned by re-serialising.
   *
   * @returns {Clip}
   */
  copy() {
    const clone = this._createInstance();
    clone.type = this.type;
    clone.name = this.name ? `${this.name} (copy)` : '';
    clone.asset = this.asset;
    clone.startTime = this.startTime;
    clone.inPoint = this.inPoint;
    clone.outPoint = this.outPoint;
    clone.muted = this.muted;
    clone.locked = this.locked;
    clone.visible = this.visible;
    clone.effects = this.effects.map((fx) => {
      // Re-hydrate from JSON to get a deep copy with a new ID.
      const json = fx.toJSON();
      const fresh = Object.assign(Object.create(Object.getPrototypeOf(fx)), json);
      fresh.id = IdGenerator.generate('fx');
      fresh.params = { ...fx.params };
      fresh.enabled = fx.enabled;
      fresh.createdAt = new Date(fx.createdAt);
      return fresh;
    });
    return clone;
  }

  /**
   * Remove this clip from its owning track.
   * @returns {boolean} True if the clip was removed.
   */
  remove() {
    if (!this._track) return false;
    return this._track.removeClip(this.id);
  }

  // ─── Convenience fade API ────────────────────────────────────────────────────

  /**
   * Attach a fade-in effect to this clip.
   * @param {number} [duration=1] - Fade duration in seconds.
   * @param {object} [options={}] - Passed through to FadeEffect.
   * @returns {Clip} this (chainable)
   */
  fadeIn(duration = 1, options = {}) {
    return this.addEffect(new FadeEffect('in', duration, options));
  }

  /**
   * Attach a fade-out effect to this clip.
   * @param {number} [duration=1]
   * @param {object} [options={}]
   * @returns {Clip} this (chainable)
   */
  fadeOut(duration = 1, options = {}) {
    return this.addEffect(new FadeEffect('out', duration, options));
  }

  /**
   * Attach a crop effect to this clip.
   * @param {object} [options={}]
   * @param {number} [options.top=0]       - Pixels to remove from the top.
   * @param {number} [options.bottom=0]    - Pixels to remove from the bottom.
   * @param {number} [options.left=0]      - Pixels to remove from the left.
   * @param {number} [options.right=0]     - Pixels to remove from the right.
   * @param {string} [options.alignment]   - CROP_ALIGNMENT value (default: 'center').
   * @returns {Clip} this (chainable)
   */
  addCrop(options = {}) {
    return this.addEffect(new CropEffect(options));
  }

  /**
   * Attach a chroma key effect to this clip, removing a solid background
   * colour (green screen, blue screen, etc.).
   * @param {object} [options={}]
   * @param {string}  [options.color='#00FF00']  - Background colour to remove.
   * @param {number}  [options.similarity=0.10]  - Colour match tolerance (0–1).
   * @param {number}  [options.blend=0.10]       - Softness of the key edge (0–1).
   * @param {boolean} [options.yuv=false]        - Match colour similarity in YUV space.
   * @returns {Clip} this (chainable)
   */
  addChromaKey(options = {}) {
    return this.addEffect(new ChromaKeyEffect(options));
  }

  // ─── Effect chain management ─────────────────────────────────────────────────

  /**
   * Append an effect to the effect chain.
   * @param {import('../effects/Effect.js').default} effect
   * @returns {Clip} this (chainable)
   */
  addEffect(effect) {
    this.effects.push(effect);
    return this;
  }

  /**
   * Remove an effect by its ID.
   * @param {string} effectId
   * @returns {boolean} True if an effect was removed.
   */
  removeEffect(effectId) {
    const idx = this.effects.findIndex((fx) => fx.id === effectId);
    if (idx === -1) return false;
    this.effects.splice(idx, 1);
    return true;
  }

  /**
   * Retrieve an effect by ID.
   * @param {string} effectId
   * @returns {import('../effects/Effect.js').default|undefined}
   */
  getEffect(effectId) {
    return this.effects.find((fx) => fx.id === effectId);
  }

  // ─── Serialisation ────────────────────────────────────────────────────────────

  /**
   * Serialise to a plain object.  Subclasses should call super.toJSON() and
   * merge their own fields.
   * @returns {object}
   */
  toJSON() {
    return {
      id: this.id,
      type: this.type,
      name: this.name,
      asset: this.asset ? this.asset.toJSON() : null,
      startTime: this.startTime,
      inPoint: this.inPoint,
      outPoint: this.outPoint,
      muted: this.muted,
      locked: this.locked,
      visible: this.visible,
      effects: this.effects.map((fx) => fx.toJSON()),
      createdAt: this.createdAt.toISOString(),
    };
  }

  // ─── Protected helpers ────────────────────────────────────────────────────────

  /**
   * Subclasses override this to return a new empty instance of their own type.
   * Used by copy() to preserve subclass identity.
   * @returns {Clip}
   */
  _createInstance() {
    // TODO: Override in every concrete subclass.
    throw new Error(`${this.constructor.name}._createInstance() is not implemented.`);
  }
}

export default Clip;
