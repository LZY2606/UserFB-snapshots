/**
 * @module InspectionService
 * Inspect VideoForge projects, timelines, and clips via the ITR layer.
 *
 * Every inspection method converts the live Project to an IntermediateTimeline
 * first.  This ensures the view is consistent with what any exporter would see,
 * and keeps the service decoupled from Project internals.
 */

import { TimelineConverter } from 'videoforge';

export class InspectionService {
  /**
   * @param {import('./ProjectService.js').ProjectService} projectService
   */
  constructor(projectService) {
    this._projects = projectService;
    this._converter = new TimelineConverter();
  }

  // ── Project ──────────────────────────────────────────────────────────────────

  /**
   * Return a high-level summary of a project.
   *
   * Includes fps, resolution, duration, track counts, clip counts,
   * caption counts, transition counts, and per-track detail.
   *
   * @param {string} projectId
   * @returns {ProjectSummary}
   */
  inspectProject(projectId) {
    const project = this._projects.getProject(projectId);
    const itr = this._converter.convert(project);

    const allClips = itr.tracks.flatMap((t) => t.clips);
    const transitionCount = allClips.reduce((n, c) => n + c.transitions.length, 0);
    const effectCount = allClips.reduce((n, c) => n + c.effects.length, 0);
    // isCaption checks type==='caption' which has no matching CLIP_TYPE constant;
    // use captionData presence as the reliable caption-clip signal instead.
    const captionClipCount = allClips.filter((c) => c.captionData !== null).length;

    const captionAnimationCount = allClips
      .filter((c) => c.captionData !== null)
      .reduce((n, c) => n + c.captionData.animations.length, 0);

    return {
      id: itr.projectId,
      name: itr.name,
      fps: itr.fps,
      width: itr.width,
      height: itr.height,
      sampleRate: itr.sampleRate,
      channels: itr.channels,
      duration: itr.computeDuration(),
      assetCount: itr.assets.length,
      trackCount: itr.tracks.length,
      videoTrackCount: itr.getVideoTracks().length,
      audioTrackCount: itr.getAudioTracks().length,
      captionTrackCount: itr.getCaptionTracks().length,
      textTrackCount: itr.getTextTracks().length,
      clipCount: allClips.length,
      captionClipCount,
      captionAnimationCount,
      transitionCount,
      effectCount,
      tracks: itr.tracks.map((t) => ({
        id: t.id,
        name: t.name,
        type: t.type,
        index: t.index,
        muted: t.muted,
        locked: t.locked,
        volume: t.volume,
        duration: t.duration,
        clipCount: t.clips.length,
      })),
    };
  }

  // ── Timeline ─────────────────────────────────────────────────────────────────

  /**
   * Return the full ITR structure for a project — tracks, assets, clips, markers.
   *
   * @param {string} projectId
   * @returns {TimelineDetail}
   */
  inspectTimeline(projectId) {
    const project = this._projects.getProject(projectId);
    const itr = this._converter.convert(project);

    return {
      projectId: itr.projectId,
      name: itr.name,
      version: itr.version,
      fps: itr.fps,
      width: itr.width,
      height: itr.height,
      sampleRate: itr.sampleRate,
      channels: itr.channels,
      duration: itr.computeDuration(),
      assets: itr.assets.map((a) => ({
        id: a.id,
        path: a.path,
        type: a.type,
        duration: a.duration,
        fps: a.fps,
        width: a.width,
        height: a.height,
      })),
      tracks: itr.tracks.map((t) => ({
        id: t.id,
        name: t.name,
        type: t.type,
        index: t.index,
        muted: t.muted,
        solo: t.solo,
        locked: t.locked,
        visible: t.visible,
        volume: t.volume,
        duration: t.duration,
        clips: t.getSortedClips().map((c) => this._clipDetail(c)),
      })),
      markers: itr.markers,
    };
  }

  // ── Track ────────────────────────────────────────────────────────────────────

  /**
   * Return detailed info for a single track by ID.
   * Uses the live Project API rather than the ITR so track properties
   * (volume, muted, solo, locked, visible) are always current.
   *
   * @param {string} projectId
   * @param {string} trackId
   * @returns {object}
   */
  inspectTrack(projectId, trackId) {
    const project = this._projects.getProject(projectId);
    const itr     = this._converter.convert(project);

    const trackRep = itr.tracks.find((t) => t.id === trackId);
    if (!trackRep) {
      throw new Error(`Track not found: "${trackId}"`);
    }

    return {
      id:        trackRep.id,
      name:      trackRep.name,
      type:      trackRep.type,
      index:     trackRep.index,
      muted:     trackRep.muted,
      solo:      trackRep.solo,
      locked:    trackRep.locked,
      visible:   trackRep.visible,
      volume:    trackRep.volume,
      duration:  trackRep.duration,
      clipCount: trackRep.clips.length,
      clips:     trackRep.getSortedClips().map((c) => this._clipDetail(c)),
    };
  }

  // ── Clip ─────────────────────────────────────────────────────────────────────

  /**
   * Inspect a single clip within a project by clip ID.
   *
   * @param {string} projectId
   * @param {string} clipId
   * @returns {ClipDetail | null}  null if the clip is not found.
   */
  inspectClip(projectId, clipId) {
    const project = this._projects.getProject(projectId);
    const itr = this._converter.convert(project);

    for (const track of itr.tracks) {
      const clip = track.clips.find((c) => c.id === clipId);
      if (clip) {
        return {
          ...this._clipDetail(clip),
          trackId: track.id,
          trackName: track.name,
          trackType: track.type,
        };
      }
    }

    return null;
  }

  // ── Internal ─────────────────────────────────────────────────────────────────

  /** @param {import('videoforge').ClipRepresentation} clip */
  _clipDetail(clip) {
    return {
      id: clip.id,
      name: clip.name,
      type: clip.type,
      assetId: clip.assetId,
      timelineStart: clip.timelineStart,
      timelineEnd: clip.timelineEnd,
      timelineDuration: clip.timelineDuration,
      sourceStart: clip.sourceStart,
      sourceEnd: clip.sourceEnd,
      sourceDuration: clip.sourceDuration,
      speed: clip.speed,
      reverse: clip.reverse,
      mute: clip.mute,
      opacity: clip.opacity,
      position: clip.position,
      scale: clip.scale,
      rotation: clip.rotation,
      effectCount: clip.effects.length,
      effects: clip.effects.map((e) => ({
        id: e.id,
        type: e.type,
        enabled: e.enabled,
        params: e.params && Object.keys(e.params).length > 0 ? e.params : undefined,
      })),
      transitionCount: clip.transitions.length,
      transitions: clip.transitions.map((t) => ({ id: t.id, type: t.type, duration: t.duration })),
      hasCaption: clip.captionData !== null && clip.captionData !== undefined,
      captionSummary: clip.captionData
        ? {
            transcript: clip.captionData.transcript?.slice(0, 120),
            segmentCount: clip.captionData.segments.length,
            animationCount: clip.captionData.animations.length,
            effectCount: clip.captionData.effects.length,
            presetName: clip.captionData.presetName,
          }
        : null,
    };
  }
}
