/**
 * @module ClipService
 * Add and remove clips across all six clip types on a VideoForge track.
 *
 * Acts as a unified factory router — callers pass clipType and the service
 * dispatches to the correct Track factory method (addVideo, addAudio, …).
 * No clip-type-specific logic bleeds into the MCP tool layer.
 */

export class ClipService {
  /**
   * @param {import('./ProjectService.js').ProjectService} projectService
   */
  constructor(projectService) {
    this._projects = projectService;
  }

  // ── Factory ───────────────────────────────────────────────────────────────────

  /**
   * Add a clip to a track.
   *
   * @param {string} projectId
   * @param {string} trackId
   * @param {string} clipType  'video' | 'audio' | 'image' | 'text' | 'shape' | 'caption'
   * @param {string} src
   *   For video/audio/image — absolute file path.
   *   For text/caption      — the text / transcript content.
   *   For shape             — shape type (rectangle | ellipse | triangle | line | polygon | arrow).
   * @param {object} [options={}]  Clip-specific options (see per-type docs below).
   * @returns {ClipSummary}
   */
  addClip(projectId, trackId, clipType, src, options = {}) {
    const project = this._projects.getProject(projectId);
    const track   = project.getTrack(trackId);
    if (!track) throw new Error(`Track not found: "${trackId}"`);

    const clip = this._create(track, clipType, src, options);
    return this._summarize(clip, trackId);
  }

  // ── Removal ───────────────────────────────────────────────────────────────────

  /**
   * Remove a clip by ID, searching all tracks in the project.
   *
   * @param {string} projectId
   * @param {string} clipId
   * @returns {{ removed: boolean, clipId: string, trackId: string }}
   */
  removeClip(projectId, clipId) {
    const project = this._projects.getProject(projectId);
    for (const track of project.getTracks()) {
      if (track.removeClip(clipId)) {
        return { removed: true, clipId, trackId: track.id };
      }
    }
    throw new Error(`Clip not found: "${clipId}"`);
  }

  // ── Internal ──────────────────────────────────────────────────────────────────

  /**
   * Route to the correct Track factory method.
   *
   * @param {import('videoforge').Track} track
   * @param {string} clipType
   * @param {string} src
   * @param {object} opts
   * @returns {import('videoforge').Clip}
   */
  _create(track, clipType, src, opts) {
    const base = this._compact({
      startTime: opts.startTime,
      inPoint:   opts.inPoint,
      outPoint:  opts.outPoint,
      name:      opts.name,
    });

    switch (clipType) {
      case 'video':
        return track.addVideo(src, {
          ...base,
          ...this._compact({
            volumeLevel:  opts.volumeLevel,
            playbackRate: opts.playbackRate,
            reversed:     opts.reversed,
          }),
        });

      case 'audio':
        return track.addAudio(src, {
          ...base,
          ...this._compact({
            volumeLevel:  opts.volumeLevel,
            panValue:     opts.panValue,
            playbackRate: opts.playbackRate,
          }),
        });

      case 'image':
        return track.addImage(src, {
          ...base,
          ...this._compact({
            x:            opts.x,
            y:            opts.y,
            scaleX:       opts.scaleX,
            scaleY:       opts.scaleY,
            rotation:     opts.rotation,
            opacityLevel: opts.opacityLevel,
          }),
        });

      case 'text':
        return track.addText(src, {
          ...base,
          ...this._compact({
            fontFamily:   opts.fontFamily,
            fontSizeValue: opts.fontSizeValue,
            colorValue:   opts.colorValue,
            bgColor:      opts.bgColor,
            alignValue:   opts.alignValue,
            bold:         opts.bold,
            italic:       opts.italic,
            x:            opts.x,
            y:            opts.y,
            opacityLevel: opts.opacityLevel,
          }),
        });

      case 'shape':
        // src = shape type ('rectangle', 'ellipse', …)
        return track.addShape(src, {
          ...base,
          ...this._compact({
            x:                opts.x,
            y:                opts.y,
            width:            opts.width,
            height:           opts.height,
            fillColor:        opts.fillColor,
            strokeColorValue: opts.strokeColorValue,
            strokeWidthValue: opts.strokeWidthValue,
            opacityLevel:     opts.opacityLevel,
            rotationDeg:      opts.rotationDeg,
            cornerRadius:     opts.cornerRadius,
          }),
        });

      case 'caption':
        return track.addCaption(src, {
          ...this._compact({
            startTime:          opts.startTime,
            outPoint:           opts.outPoint,
            name:               opts.name,
            preset:             opts.preset,
            maxWordsPerSegment: opts.maxWordsPerSegment,
          }),
        });

      default:
        throw new Error(
          `Unknown clipType: "${clipType}". ` +
          'Use: video, audio, image, text, shape, caption',
        );
    }
  }

  /**
   * Build a lightweight summary from a live Clip instance.
   *
   * @param {import('videoforge').Clip} clip
   * @param {string} trackId
   * @returns {ClipSummary}
   */
  _summarize(clip, trackId) {
    return {
      clipId:    clip.id,
      type:      clip.type,
      name:      clip.name,
      trackId,
      startTime: clip.startTime,
      duration:  clip.duration,
      endTime:   clip.endTime,
      inPoint:   clip.inPoint,
      outPoint:  clip.outPoint,
    };
  }

  /** Strip undefined so constructor defaults are not overridden. */
  _compact(obj) {
    return Object.fromEntries(
      Object.entries(obj).filter(([, v]) => v !== undefined),
    );
  }
}

/**
 * @typedef {{
 *   clipId: string, type: string, name: string, trackId: string,
 *   startTime: number, duration: number, endTime: number,
 *   inPoint: number, outPoint: number
 * }} ClipSummary
 */
