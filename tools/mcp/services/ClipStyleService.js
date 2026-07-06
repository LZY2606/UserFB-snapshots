/**
 * @module ClipStyleService
 * Per-type style/media controls: video, audio, image, text, shape.
 */

export class ClipStyleService {
  /**
   * @param {import('./ProjectService.js').ProjectService} projectService
   */
  constructor(projectService) {
    this._projects = projectService;
  }

  // ── Internal ──────────────────────────────────────────────────────────────────

  _requireClip(project, clipId) {
    for (const track of project.getTracks()) {
      const clip = track.getClip(clipId);
      if (clip) return clip;
    }
    throw new Error(`Clip not found: "${clipId}"`);
  }

  // ── Video ─────────────────────────────────────────────────────────────────────

  /**
   * Set video clip playback/audio options.
   * @param {string} projectId
   * @param {string} clipId
   * @param {object} options  { volumeLevel?, muted?, playbackRate?, reversed? }
   */
  setVideoOptions(projectId, clipId, options = {}) {
    const project = this._projects.getProject(projectId);
    const clip    = this._requireClip(project, clipId);

    if (options.volumeLevel !== undefined) clip.volume(options.volumeLevel);
    if (options.muted       !== undefined) options.muted ? clip.mute() : clip.unmute();
    if (options.playbackRate !== undefined) clip.speed(options.playbackRate);
    if (options.reversed    !== undefined) clip.reverse(options.reversed);

    return {
      clipId: clip.id,
      volume:   clip.volume(),
      muted:    clip.isMuted,
      speed:    clip.speed(),
      reversed: clip.isReversed,
    };
  }

  // ── Audio ─────────────────────────────────────────────────────────────────────

  /**
   * Set audio clip controls.
   * @param {string} projectId
   * @param {string} clipId
   * @param {object} options  { volumeLevel?, panValue?, playbackRate?, muted? }
   */
  setAudioOptions(projectId, clipId, options = {}) {
    const project = this._projects.getProject(projectId);
    const clip    = this._requireClip(project, clipId);

    if (options.volumeLevel  !== undefined) clip.volume(options.volumeLevel);
    if (options.panValue     !== undefined) clip.pan(options.panValue);
    if (options.playbackRate !== undefined) clip.speed(options.playbackRate);
    if (options.muted        !== undefined) options.muted ? clip.mute() : clip.unmute();

    return {
      clipId:  clip.id,
      volume:  clip.volume(),
      pan:     clip.pan(),
      speed:   clip.speed(),
      muted:   clip.isMuted,
    };
  }

  // ── Image ─────────────────────────────────────────────────────────────────────

  /**
   * Set image clip transform properties.
   * @param {string} projectId
   * @param {string} clipId
   * @param {object} options  { x?, y?, scaleX?, scaleY?, rotation?, opacityLevel? }
   */
  setImageTransform(projectId, clipId, options = {}) {
    const project = this._projects.getProject(projectId);
    const clip    = this._requireClip(project, clipId);

    if (options.x !== undefined || options.y !== undefined) {
      const cur = clip.position();
      clip.position(options.x ?? cur.x, options.y ?? cur.y);
    }
    if (options.scaleX !== undefined || options.scaleY !== undefined) {
      const cur = clip.scale();
      clip.scale(options.scaleX ?? cur.x, options.scaleY ?? cur.y);
    }
    if (options.rotation    !== undefined) clip.rotation(options.rotation);
    if (options.opacityLevel !== undefined) clip.opacity(options.opacityLevel);

    return {
      clipId:   clip.id,
      position: clip.position(),
      scale:    clip.scale(),
      rotation: clip.rotation(),
      opacity:  clip.opacity(),
    };
  }

  // ── Text ──────────────────────────────────────────────────────────────────────

  /**
   * Set text clip style properties.
   * @param {string} projectId
   * @param {string} clipId
   * @param {object} options  { fontFamily?, fontSizeValue?, colorValue?, bgColor?,
   *                            alignValue?, bold?, italic?, x?, y?, opacityLevel? }
   */
  setTextStyle(projectId, clipId, options = {}) {
    const project = this._projects.getProject(projectId);
    const clip    = this._requireClip(project, clipId);

    if (options.fontFamily   !== undefined) clip.font(options.fontFamily);
    if (options.fontSizeValue !== undefined) clip.fontSize(options.fontSizeValue);
    if (options.colorValue   !== undefined) clip.color(options.colorValue);
    if (options.bgColor      !== undefined) clip.background(options.bgColor);
    if (options.alignValue   !== undefined) clip.align(options.alignValue);
    if (options.bold         !== undefined) clip.bold(options.bold);
    if (options.italic       !== undefined) clip.italic(options.italic);
    if (options.opacityLevel !== undefined) clip.opacity(options.opacityLevel);
    if (options.x !== undefined || options.y !== undefined) {
      const cur = clip.position();
      clip.position(options.x ?? cur.x, options.y ?? cur.y);
    }

    return {
      clipId:     clip.id,
      font:       clip.font(),
      fontSize:   clip.fontSize(),
      color:      clip.color(),
      background: clip.background(),
      align:      clip.align(),
      bold:       clip.bold(),
      italic:     clip.italic(),
      position:   clip.position(),
      opacity:    clip.opacity(),
    };
  }

  // ── Shape ─────────────────────────────────────────────────────────────────────

  /**
   * Set shape clip style properties.
   * @param {string} projectId
   * @param {string} clipId
   * @param {object} options  { x?, y?, width?, height?, fillColor?,
   *                            strokeColorValue?, strokeWidthValue?,
   *                            rotationDeg?, cornerRadius?, opacityLevel? }
   */
  setShapeStyle(projectId, clipId, options = {}) {
    const project = this._projects.getProject(projectId);
    const clip    = this._requireClip(project, clipId);

    if (options.x !== undefined || options.y !== undefined) {
      const cur = clip.position();
      clip.position(options.x ?? cur.x, options.y ?? cur.y);
    }
    if (options.width !== undefined || options.height !== undefined) {
      const cur = clip.size();
      clip.size(options.width ?? cur.width, options.height ?? cur.height);
    }
    if (options.fillColor        !== undefined) clip.fillColor(options.fillColor);
    if (options.strokeColorValue !== undefined) clip.strokeColor(options.strokeColorValue);
    if (options.strokeWidthValue !== undefined) clip.strokeWidth(options.strokeWidthValue);
    if (options.rotationDeg      !== undefined) clip.rotation(options.rotationDeg);
    if (options.cornerRadius     !== undefined) clip.cornerRadius(options.cornerRadius);
    if (options.opacityLevel     !== undefined) clip.opacity(options.opacityLevel);

    return {
      clipId:       clip.id,
      position:     clip.position(),
      size:         clip.size(),
      fillColor:    clip.fillColor(),
      strokeColor:  clip.strokeColor(),
      strokeWidth:  clip.strokeWidth(),
      rotation:     clip.rotation(),
      cornerRadius: clip.cornerRadius(),
      opacity:      clip.opacity(),
    };
  }
}
