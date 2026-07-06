/**
 * @module TrackService
 * Add, remove, reorder, and inspect tracks on a VideoForge project.
 *
 * Thin wrapper over the Project track management API.
 * All track mutations go through this service so the rest of the MCP
 * layer never reaches into Project._tracks directly.
 */

export class TrackService {
  /**
   * @param {import('./ProjectService.js').ProjectService} projectService
   */
  constructor(projectService) {
    this._projects = projectService;
  }

  // ── Mutations ─────────────────────────────────────────────────────────────────

  /**
   * Add a new track to a project.
   *
   * @param {string} projectId
   * @param {string} type  - 'video' | 'audio' | 'image' | 'text' | 'shape'
   * @param {object} [options={}]
   * @param {string}  [options.name]
   * @param {number}  [options.volume]
   * @param {boolean} [options.muted]
   * @param {boolean} [options.solo]
   * @param {boolean} [options.locked]
   * @param {boolean} [options.visible]
   * @returns {{ trackId: string, name: string, type: string, index: number }}
   */
  addTrack(projectId, type, options = {}) {
    const project = this._projects.getProject(projectId);
    const track = project.addTrack(type, this._compact(options));
    const index = project.getTracks().findIndex((t) => t.id === track.id);

    return {
      trackId: track.id,
      name:    track.name,
      type:    track.type,
      index,
    };
  }

  /**
   * Remove a track from a project.
   *
   * @param {string} projectId
   * @param {string} trackId
   * @returns {{ removed: boolean, trackId: string }}
   */
  removeTrack(projectId, trackId) {
    const project = this._projects.getProject(projectId);
    const removed = project.removeTrack(trackId);
    if (!removed) {
      throw new Error(`Track not found: "${trackId}"`);
    }
    return { removed: true, trackId };
  }

  /**
   * Reorder tracks by supplying the desired track ID array.
   * IDs absent from orderedIds are appended at the end in their original order.
   *
   * @param {string}   projectId
   * @param {string[]} orderedIds
   * @returns {Array<{ id: string, name: string, type: string, index: number }>}
   */
  reorderTracks(projectId, orderedIds) {
    const project = this._projects.getProject(projectId);
    project.reorderTracks(orderedIds);
    return project.getTracks().map((t, i) => ({
      id:    t.id,
      name:  t.name,
      type:  t.type,
      index: i,
    }));
  }

  // ── Inspection ────────────────────────────────────────────────────────────────

  /**
   * Return detailed info for a single track, including all its clips.
   *
   * @param {string} projectId
   * @param {string} trackId
   * @returns {TrackDetail}
   */
  inspectTrack(projectId, trackId) {
    const project = this._projects.getProject(projectId);
    const track = project.getTrack(trackId);

    if (!track) {
      throw new Error(`Track not found: "${trackId}"`);
    }

    const clips = track.getClips();

    return {
      id:        track.id,
      name:      track.name,
      type:      track.type,
      volume:    track.volume,
      muted:     track.muted,
      solo:      track.solo,
      locked:    track.locked,
      visible:   track.visible,
      duration:  track.getDuration(),
      clipCount: clips.length,
      clips: clips.map((c) => ({
        id:        c.id,
        name:      c.name,
        type:      c.type,
        startTime: c.startTime,
        duration:  c.duration,
        endTime:   c.endTime,
        inPoint:   c.inPoint,
        outPoint:  c.outPoint,
        muted:     c.muted,
        locked:    c.locked,
      })),
    };
  }

  // ── Internal ──────────────────────────────────────────────────────────────────

  /** Strip undefined values so they don't override constructor defaults. */
  _compact(obj) {
    return Object.fromEntries(
      Object.entries(obj).filter(([, v]) => v !== undefined),
    );
  }
}
