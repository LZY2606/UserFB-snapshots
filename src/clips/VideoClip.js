/**
 * @module VideoClip
 * A clip backed by a video asset.  Extends Clip with playback controls
 * (volume, speed, mute, reverse) specific to video/audio tracks.
 */

import Clip from '../core/Clip.js';
import Asset from '../core/Asset.js';
import { CLIP_TYPES, ASSET_TYPES } from '../utils/Constants.js';

class VideoClip extends Clip {
  /**
   * @param {Asset|null} asset
   * @param {object} [options={}]
   * @param {number} [options.startTime=0]
   * @param {number} [options.inPoint=0]
   * @param {number} [options.outPoint=null]
   * @param {number} [options.volumeLevel=1] - Initial volume (0–2).
   * @param {number} [options.playbackRate=1] - Initial speed multiplier.
   * @param {boolean} [options.reversed=false]
   */
  constructor(asset, options = {}) {
    super(asset, { ...options, type: CLIP_TYPES.VIDEO });

    /** @type {number} Volume multiplier 0–2 (1 = original). */
    this._volumeLevel = options.volumeLevel ?? 1;

    /** @type {boolean} */
    this._muted = options.muted ?? false;

    /** @type {number} Playback rate multiplier (0.25–16). */
    this._playbackRate = options.playbackRate ?? 1;

    /** @type {boolean} */
    this._reversed = options.reversed ?? false;
  }

  // ─── Video-specific controls ─────────────────────────────────────────────────

  /**
   * Set or get the volume level.
   * @param {number} [value] - If provided, sets volume (0–2). Omit to read.
   * @returns {number|VideoClip} Current volume, or this (chainable).
   */
  volume(value) {
    if (value === undefined) return this._volumeLevel;
    if (value < 0 || value > 2) throw new RangeError('Volume must be in range [0, 2]');
    this._volumeLevel = value;
    this._muted = false;
    return this;
  }

  /**
   * Silence this clip without changing the stored volume level.
   * @returns {VideoClip} this (chainable)
   */
  mute() {
    this._muted = true;
    return this;
  }

  /**
   * Restore audio after mute().
   * @returns {VideoClip} this (chainable)
   */
  unmute() {
    this._muted = false;
    return this;
  }

  /**
   * Set or get the playback speed.
   * @param {number} [rate] - Multiplier (e.g. 2 = 2× speed, 0.5 = half speed).
   * @returns {number|VideoClip}
   */
  speed(rate) {
    if (rate === undefined) return this._playbackRate;
    if (rate <= 0) throw new RangeError('Playback rate must be > 0');
    this._playbackRate = rate;
    return this;
  }

  /**
   * Toggle reversed playback.
   * @param {boolean} [value] - If omitted, toggles current state.
   * @returns {VideoClip} this (chainable)
   */
  reverse(value) {
    this._reversed = value !== undefined ? Boolean(value) : !this._reversed;
    return this;
  }

  /** @returns {boolean} Whether the clip plays in reverse. */
  get isReversed() {
    return this._reversed;
  }

  /** @returns {boolean} */
  get isMuted() {
    return this._muted;
  }

  // ─── Clock ────────────────────────────────────────────────────────────────────

  /**
   * Playback duration on the timeline (seconds): the trimmed source length
   * divided by the playback rate.
   * @returns {number}
   */
  get duration() {
    return (this.outPoint - this.inPoint) / this._playbackRate;
  }

  /**
   * Map a timeline-absolute time to the corresponding source-media time.
   * Forward playback samples from the in-point onwards; reversed playback
   * samples from the out-point backwards (the first timeline instant maps
   * to exactly `outPoint`).
   *
   * @param {number} time - Timeline position in seconds.
   * @returns {number} Source-media time in seconds.
   */
  getSourceTimeAt(time) {
    const offset = (time - this.startTime) * this._playbackRate;
    return this._reversed ? this.outPoint - offset : this.inPoint + offset;
  }

  /** @protected */
  _splitSourceRange(time) {
    const offset = (time - this.startTime) * this._playbackRate;
    if (this._reversed) {
      // Reversed: source time decreases along the timeline, so the head
      // keeps the out-point side and the tail keeps the in-point side.
      const cut = this.outPoint - offset;
      return { headIn: cut, headOut: this.outPoint, tailIn: this.inPoint, tailOut: cut };
    }
    const cut = this.inPoint + offset;
    return { headIn: this.inPoint, headOut: cut, tailIn: cut, tailOut: this.outPoint };
  }

  // ─── Protected ────────────────────────────────────────────────────────────────

  _createInstance() {
    return new VideoClip(null, {
      volumeLevel: this._volumeLevel,
      muted: this._muted,
      playbackRate: this._playbackRate,
      reversed: this._reversed,
    });
  }

  // ─── Serialisation ────────────────────────────────────────────────────────────

  toJSON() {
    return {
      ...super.toJSON(),
      volumeLevel: this._volumeLevel,
      muted: this._muted,
      playbackRate: this._playbackRate,
      reversed: this._reversed,
    };
  }

  /**
   * @param {object} data
   * @returns {VideoClip}
   */
  static fromJSON(data) {
    const asset = data.asset ? Asset.fromJSON(data.asset) : null;
    const clip = new VideoClip(asset, {
      startTime: data.startTime,
      inPoint: data.inPoint,
      outPoint: data.outPoint,
      volumeLevel: data.volumeLevel,
      muted: data.muted,
      playbackRate: data.playbackRate,
      reversed: data.reversed,
    });
    clip.id = data.id;
    clip.name = data.name;
    clip.locked = data.locked;
    clip.visible = data.visible;
    clip.createdAt = new Date(data.createdAt);
    // TODO: Rehydrate effects array from data.effects using Effect.fromJSON registry.
    return clip;
  }
}

export default VideoClip;
