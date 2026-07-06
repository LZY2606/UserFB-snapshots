/**
 * @module ClipEditingService
 * Clip-level editing operations: trim, move, split, copy, fade, transition.
 */

import { Transition, TRANSITION_TYPES, EASING, CropEffect, CROP_ALIGNMENT } from 'videoforge';

export class ClipEditingService {
  /**
   * @param {import('./ProjectService.js').ProjectService} projectService
   */
  constructor(projectService) {
    this._projects = projectService;
  }

  // ── Internal ──────────────────────────────────────────────────────────────────

  /** Search every track for clipId; throws if not found. */
  _requireClip(project, clipId) {
    for (const track of project.getTracks()) {
      const clip = track.getClip(clipId);
      if (clip) return { clip, track };
    }
    throw new Error(`Clip not found: "${clipId}"`);
  }

  // ── Operations ───────────────────────────────────────────────────────────────

  /**
   * Trim a clip's source in/out points.
   * @param {string} projectId
   * @param {string} clipId
   * @param {number} inPoint  seconds
   * @param {number} outPoint seconds
   */
  trimClip(projectId, clipId, inPoint, outPoint) {
    const project = this._projects.getProject(projectId);
    const { clip, track } = this._requireClip(project, clipId);

    clip.trim(inPoint, outPoint);

    return {
      clipId: clip.id,
      trackId: track.id,
      inPoint: clip.inPoint,
      outPoint: clip.outPoint,
      duration: clip.duration,
      timelineStart: clip.startTime,
      timelineEnd: clip.endTime,
    };
  }

  /**
   * Move a clip to a new timeline start position.
   * @param {string} projectId
   * @param {string} clipId
   * @param {number} startTime seconds
   */
  moveClip(projectId, clipId, startTime) {
    const project = this._projects.getProject(projectId);
    const { clip, track } = this._requireClip(project, clipId);

    clip.move(startTime);

    return {
      clipId: clip.id,
      trackId: track.id,
      timelineStart: clip.startTime,
      timelineEnd: clip.endTime,
      duration: clip.duration,
    };
  }

  /**
   * Split a clip at an absolute timeline time.
   * Returns IDs + timing for both head and tail.
   * @param {string} projectId
   * @param {string} clipId
   * @param {number} splitTime seconds (absolute timeline position)
   */
  splitClip(projectId, clipId, splitTime) {
    const project = this._projects.getProject(projectId);
    const { clip, track } = this._requireClip(project, clipId);

    const { head, tail } = clip.split(splitTime);

    return {
      trackId: track.id,
      head: {
        id: head.id,
        timelineStart: head.startTime,
        timelineEnd: head.endTime,
        duration: head.duration,
        inPoint: head.inPoint,
        outPoint: head.outPoint,
      },
      tail: {
        id: tail.id,
        timelineStart: tail.startTime,
        timelineEnd: tail.endTime,
        duration: tail.duration,
        inPoint: tail.inPoint,
        outPoint: tail.outPoint,
      },
    };
  }

  /**
   * Copy a clip (deep clone with new ID).  Optionally reposition the copy.
   * @param {string} projectId
   * @param {string} clipId
   * @param {number|undefined} startTime  If provided, move the copy here.
   */
  copyClip(projectId, clipId, startTime) {
    const project = this._projects.getProject(projectId);
    const { clip, track } = this._requireClip(project, clipId);

    const copy = clip.copy();
    if (startTime !== undefined) {
      copy.move(startTime);
    }

    return {
      originalClipId: clipId,
      newClipId: copy.id,
      trackId: track.id,
      timelineStart: copy.startTime,
      timelineEnd: copy.endTime,
      duration: copy.duration,
    };
  }

  /**
   * Add a fade-in or fade-out effect to a clip.
   * @param {string} projectId
   * @param {string} clipId
   * @param {'in'|'out'} direction
   * @param {number} duration  seconds
   * @param {object} options   { easing? }
   */
  addFade(projectId, clipId, direction, duration = 1, options = {}) {
    const project = this._projects.getProject(projectId);
    const { clip } = this._requireClip(project, clipId);

    const fadeOptions = {};
    if (options.easing) fadeOptions.easing = options.easing;

    if (direction === 'in') {
      clip.fadeIn(duration, fadeOptions);
    } else if (direction === 'out') {
      clip.fadeOut(duration, fadeOptions);
    } else {
      throw new Error(`Invalid fade direction "${direction}". Must be "in" or "out".`);
    }

    return {
      clipId: clip.id,
      direction,
      duration,
      totalEffects: clip.effects.length,
    };
  }

  /**
   * Add a transition effect to a clip.
   * The transition is attached to clipId.  fromClipId / toClipId are optional
   * linking metadata that help exporters place the transition correctly.
   *
   * @param {string} projectId
   * @param {string} clipId        The clip receiving the transition effect.
   * @param {string} transitionType  One of TRANSITION_TYPES values.
   * @param {number} duration      seconds
   * @param {object} options       { easing?, fromClipId?, toClipId? }
   */
  addTransition(projectId, clipId, transitionType, duration = 1, options = {}) {
    const project = this._projects.getProject(projectId);
    const { clip } = this._requireClip(project, clipId);

    // Validate transitionType
    const validTypes = Object.values(TRANSITION_TYPES);
    if (!validTypes.includes(transitionType)) {
      throw new Error(
        `Unknown transition type "${transitionType}". Valid types: ${validTypes.join(', ')}`
      );
    }

    const transitionOptions = {};
    if (options.easing)      transitionOptions.easing      = options.easing;
    if (options.fromClipId)  transitionOptions.fromClipId  = options.fromClipId;
    if (options.toClipId)    transitionOptions.toClipId    = options.toClipId;

    const transition = new Transition(transitionType, duration, transitionOptions);

    // Link if both adjacent clips are specified
    if (options.fromClipId && options.toClipId) {
      let fromClip, toClip;
      for (const track of project.getTracks()) {
        fromClip = fromClip || track.getClip(options.fromClipId);
        toClip   = toClip   || track.getClip(options.toClipId);
      }
      if (fromClip && toClip) transition.link(fromClip, toClip);
    }

    clip.addEffect(transition);

    return {
      clipId: clip.id,
      transitionId: transition.id,
      transitionType,
      duration,
      totalEffects: clip.effects.length,
    };
  }

  /**
   * Add a CropEffect to a clip, removing pixels from one or more edges.
   * Uses the Clip.addCrop() convenience method added in v0.9.0-alpha.2.
   *
   * @param {string} projectId
   * @param {string} clipId
   * @param {object} options  { top?, bottom?, left?, right?, alignment? }
   */
  addCrop(projectId, clipId, options = {}) {
    const project = this._projects.getProject(projectId);
    const { clip } = this._requireClip(project, clipId);

    const cropOptions = {};
    if (options.top       !== undefined) cropOptions.top       = options.top;
    if (options.bottom    !== undefined) cropOptions.bottom    = options.bottom;
    if (options.left      !== undefined) cropOptions.left      = options.left;
    if (options.right     !== undefined) cropOptions.right     = options.right;
    if (options.alignment !== undefined) cropOptions.alignment = options.alignment;

    clip.addCrop(cropOptions);

    const effect = clip.effects[clip.effects.length - 1];
    return {
      clipId:       clip.id,
      effectId:     effect.id,
      type:         'crop',
      params:       effect.params,
      totalEffects: clip.effects.length,
    };
  }

  /**
   * Remove any effect (fade, transition, crop, etc.) from a clip by effect ID.
   *
   * @param {string} projectId
   * @param {string} clipId
   * @param {string} effectId
   */
  removeClipEffect(projectId, clipId, effectId) {
    const project = this._projects.getProject(projectId);
    const { clip } = this._requireClip(project, clipId);

    const removed = clip.removeEffect(effectId);
    if (!removed) throw new Error(`Effect not found: "${effectId}"`);

    return {
      clipId:       clip.id,
      effectId,
      removed:      true,
      totalEffects: clip.effects.length,
    };
  }
}
