/**
 * @module PreviewRenderer
 * Composites a single timeline frame from the active clip stack.
 *
 * PreviewRenderer is environment-agnostic by design: the actual pixel
 * operations are delegated to a pluggable `backend` (CpuBackend, Canvas2D,
 * WebGL, node-canvas, OffscreenCanvas, etc.).  This class provides the
 * compositing logic and effect pipeline dispatch.
 */

import Transition from '../effects/Transition.js';
import { EFFECT_TYPES, TRACK_TYPES, CLIP_TYPES } from '../utils/Constants.js';

/** Tolerance when comparing the transition duration to the clip overlap. */
const OVERLAP_EPSILON = 1e-6;

class PreviewRenderer {
  /**
   * @param {import('../core/Timeline.js').default} timeline
   * @param {object} [options={}]
   * @param {object} [options.backend=null] - Rendering backend instance.
   *        Expected interface: { clear(), drawVideo(clip, sourceTime),
   *          drawTransition(layer)?, getFrame() }
   */
  constructor(timeline, options = {}) {
    /** @type {import('../core/Timeline.js').default} */
    this.timeline = timeline;

    /** @type {object|null} */
    this.backend = options.backend ?? null;

    /** @type {number} Last rendered frame number. */
    this._lastFrame = -1;

    /** @type {boolean} */
    this._rendering = false;
  }

  // ─── Core render API ─────────────────────────────────────────────────────────

  /**
   * Composite all active clips at `time` and return a frame.
   *
   * Pipeline:
   *   1. Clear the backend (every render starts from transparent).
   *   2. Walk tracks bottom → top (Project.getTracks() order); hidden
   *      tracks and non-video tracks are skipped.
   *   3. Within a track, clips active at `time` (half-open interval
   *      [startTime, endTime)) are drawn in getClips() order.  Two
   *      overlapping clips joined by an enabled Transition on the outgoing
   *      clip collapse into a single transition layer for the whole
   *      overlap — they are never drawn individually inside it.
   *   4. Only VideoClips are drawn; every other clip type is skipped.
   *
   * @param {number} time - Timeline position in seconds.
   * @returns {Promise<object>} Resolves to a backend-specific frame object.
   */
  async render(time) {
    if (this._rendering) {
      throw new Error('PreviewRenderer.render() called while a render is already in progress.');
    }

    this._rendering = true;
    try {
      this._ensureBackend();
      await this.backend.clear();

      const tracks = this.timeline._project ? this.timeline._project.getTracks() : [];
      for (const track of tracks) {
        if (track.visible === false) continue;
        if (track.type !== TRACK_TYPES.VIDEO) continue;
        for (const layer of this._layersAt(track, time)) {
          if (layer.kind === 'transition') {
            await this._drawTransitionLayer(layer);
          } else {
            await this._drawClipLayer(layer);
          }
        }
      }

      this._lastFrame = this.timeline.timeToFrame(time);
      return await this.backend.getFrame();
    } finally {
      this._rendering = false;
    }
  }

  /**
   * Render every frame in the range [startTime, endTime) and invoke the
   * callback with each frame.
   *
   * @param {number} startTime
   * @param {number} endTime
   * @param {function(object, number): Promise<void>} onFrame - Called per frame with (frame, frameNumber).
   * @returns {Promise<void>}
   */
  async renderRange(startTime, endTime, onFrame) {
    const fps = this.timeline.fps;
    const startFrame = this.timeline.timeToFrame(startTime);
    const endFrame = this.timeline.timeToFrame(endTime);

    for (let f = startFrame; f < endFrame; f++) {
      const t = this.timeline.frameToTime(f);
      const frame = await this.render(t);
      await onFrame(frame, f);
    }
  }

  /**
   * Return the composited frame at `time` without invoking a full render cycle.
   * Intended for thumbnail / seek-preview use.
   *
   * @param {number} time
   * @returns {Promise<object>}
   */
  async getFrame(time) {
    return this.render(time);
  }

  // ─── Compositing internals ──────────────────────────────────────────────────

  /**
   * Build the ordered layer list for one track at `time`.
   *
   * Layers are either single clips (`{ kind: 'clip', clip, sourceTime }`) or
   * transition pairs (`{ kind: 'transition', transition, fromClip, ... }`).
   * Overlapping clips without an enabled transition simply stack in
   * getClips() order.
   *
   * @param {import('../core/Track.js').default} track
   * @param {number} time
   * @returns {object[]}
   */
  _layersAt(track, time) {
    const clips = track.getClips().filter((clip) =>
      clip.type === CLIP_TYPES.VIDEO &&
      clip.visible !== false &&
      clip.startTime <= time && clip.endTime > time);

    const layers = [];
    let i = 0;
    while (i < clips.length) {
      const from = clips[i];
      const to = clips[i + 1];
      if (to && from.endTime > to.startTime) {
        const transition = PreviewRenderer._transitionBetween(from, to);
        if (transition) {
          layers.push({
            kind: 'transition',
            transition,
            fromClip: from,
            fromSourceTime: from.getSourceTimeAt(time),
            toClip: to,
            toSourceTime: to.getSourceTimeAt(time),
            elapsed: time - to.startTime,
          });
          i += 2;
          continue;
        }
      }
      layers.push({ kind: 'clip', clip: from, sourceTime: from.getSourceTimeAt(time) });
      i += 1;
    }
    return layers;
  }

  /**
   * Find the enabled Transition on the outgoing clip that links the
   * overlapping pair `(from, to)`.  Returns null when none applies
   * (including disabled transitions, which are treated as a plain
   * overlap).  Throws when a linked transition's clip IDs do not match the
   * pair or its duration does not equal the overlap length.
   *
   * @param {import('../core/Clip.js').default} from - Outgoing (earlier) clip.
   * @param {import('../core/Clip.js').default} to   - Incoming (later) clip.
   * @returns {Transition|null}
   */
  static _transitionBetween(from, to) {
    for (const fx of from.effects ?? []) {
      const isTransition = fx instanceof Transition || fx?.type === EFFECT_TYPES.TRANSITION;
      if (!isTransition) continue;

      const fromId = fx.params?.fromClipId ?? null;
      const toId = fx.params?.toClipId ?? null;
      const involvesPair = fromId === from.id || toId === from.id ||
                           fromId === to.id || toId === to.id;
      if (!involvesPair) continue;
      if (fx.enabled === false) continue; // disabled → plain overlap

      if (fromId !== from.id || toId !== to.id) {
        throw new Error(
          `Transition ${fx.id} links (${fromId}, ${toId}) but the overlapping ` +
          `clips are (${from.id}, ${to.id}).`,
        );
      }
      const overlap = from.endTime - to.startTime;
      if (Math.abs(overlap - fx.duration) > OVERLAP_EPSILON) {
        throw new Error(
          `Transition ${fx.id} duration (${fx.duration}s) does not match the ` +
          `overlap between the linked clips (${overlap}s).`,
        );
      }
      return fx;
    }
    return null;
  }

  async _drawClipLayer(layer) {
    if (typeof this.backend.drawVideo === 'function') {
      await this.backend.drawVideo(layer.clip, layer.sourceTime);
    }
  }

  async _drawTransitionLayer(layer) {
    if (typeof this.backend.drawTransition === 'function') {
      await this.backend.drawTransition(layer);
      return;
    }
    // Backends without transition support stack both clips in order.
    await this._drawClipLayer({ clip: layer.fromClip, sourceTime: layer.fromSourceTime });
    await this._drawClipLayer({ clip: layer.toClip, sourceTime: layer.toSourceTime });
  }

  // ─── Backend management ───────────────────────────────────────────────────────

  /**
   * Attach or replace the rendering backend.
   * @param {object} backend
   * @returns {PreviewRenderer} this (chainable)
   */
  setBackend(backend) {
    this.backend = backend;
    return this;
  }

  _ensureBackend() {
    if (!this.backend) {
      throw new Error(
        'PreviewRenderer has no backend. Call setBackend() before rendering.\n' +
        'Available backends: Canvas2DBackend (browser), NodeCanvasBackend (Node.js), WebGLBackend.'
      );
    }
  }
}

export default PreviewRenderer;
