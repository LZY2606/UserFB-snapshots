/**
 * @module AudioClip
 * A clip backed by an audio-only asset.
 * Provides volume, pan, speed, and mute controls.
 */

import Clip from '../core/Clip.js';
import Asset from '../core/Asset.js';
import { CLIP_TYPES } from '../utils/Constants.js';

class AudioClip extends Clip {
  /**
   * @param {Asset|null} asset
   * @param {object} [options={}]
   * @param {number} [options.startTime=0]
   * @param {number} [options.inPoint=0]
   * @param {number} [options.outPoint=null]
   * @param {number} [options.volumeLevel=1]   - Volume multiplier 0–2.
   * @param {number} [options.panValue=0]      - Stereo pan -1 (left) to +1 (right).
   * @param {number} [options.playbackRate=1]
   * @param {boolean}[options.muted=false]
   */
  constructor(asset, options = {}) {
    super(asset, { ...options, type: CLIP_TYPES.AUDIO });

    /** @type {number} */
    this._volumeLevel = options.volumeLevel ?? 1;

    /** @type {number} Stereo pan: -1 = hard left, 0 = center, 1 = hard right. */
    this._panValue = options.panValue ?? 0;

    /** @type {number} */
    this._playbackRate = options.playbackRate ?? 1;

    /** @type {boolean} */
    this._muted = options.muted ?? false;
  }

  // ─── Audio controls ───────────────────────────────────────────────────────────

  /**
   * Get or set volume.
   * @param {number} [value]
   * @returns {number|AudioClip}
   */
  volume(value) {
    if (value === undefined) return this._volumeLevel;
    if (value < 0 || value > 2) throw new RangeError('Volume must be in range [0, 2]');
    this._volumeLevel = value;
    this._muted = false;
    return this;
  }

  /**
   * Get or set stereo pan.
   * @param {number} [value] - -1 to +1.
   * @returns {number|AudioClip}
   */
  pan(value) {
    if (value === undefined) return this._panValue;
    if (value < -1 || value > 1) throw new RangeError('Pan must be in range [-1, 1]');
    this._panValue = value;
    return this;
  }

  /**
   * Get or set playback speed.
   * @param {number} [rate]
   * @returns {number|AudioClip}
   */
  speed(rate) {
    if (rate === undefined) return this._playbackRate;
    if (rate <= 0) throw new RangeError('Playback rate must be > 0');
    this._playbackRate = rate;
    return this;
  }

  /**
   * Mute this clip.
   * @returns {AudioClip} this (chainable)
   */
  mute() {
    this._muted = true;
    return this;
  }

  /**
   * Unmute this clip.
   * @returns {AudioClip} this (chainable)
   */
  unmute() {
    this._muted = false;
    return this;
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
   * @param {number} time - Timeline position in seconds.
   * @returns {number} Source-media time in seconds.
   */
  getSourceTimeAt(time) {
    return this.inPoint + (time - this.startTime) * this._playbackRate;
  }

  /** @protected */
  _splitSourceRange(time) {
    const cut = this.inPoint + (time - this.startTime) * this._playbackRate;
    return { headIn: this.inPoint, headOut: cut, tailIn: cut, tailOut: this.outPoint };
  }

  // ─── Protected ────────────────────────────────────────────────────────────────

  _createInstance() {
    return new AudioClip(null, {
      volumeLevel: this._volumeLevel,
      panValue: this._panValue,
      playbackRate: this._playbackRate,
      muted: this._muted,
    });
  }

  // ─── Serialisation ────────────────────────────────────────────────────────────

  toJSON() {
    return {
      ...super.toJSON(),
      volumeLevel: this._volumeLevel,
      panValue: this._panValue,
      playbackRate: this._playbackRate,
      muted: this._muted,
    };
  }

  /**
   * @param {object} data
   * @returns {AudioClip}
   */
  static fromJSON(data) {
    const asset = data.asset ? Asset.fromJSON(data.asset) : null;
    const clip = new AudioClip(asset, {
      startTime: data.startTime,
      inPoint: data.inPoint,
      outPoint: data.outPoint,
      volumeLevel: data.volumeLevel,
      panValue: data.panValue,
      playbackRate: data.playbackRate,
      muted: data.muted,
    });
    clip.id = data.id;
    clip.name = data.name;
    clip.locked = data.locked;
    clip.visible = data.visible;
    clip.createdAt = new Date(data.createdAt);
    // TODO: Rehydrate effects.
    return clip;
  }
}

export default AudioClip;
