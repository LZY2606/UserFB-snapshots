/**
 * @module CaptionService
 * Caption engine: transcript, presets, style, layout, animations, effects, keyframes.
 *
 * Covers Phase 4 (core) and Phase 5 (advanced) caption operations.
 */

import { ANIMATION_REGISTRY, EFFECT_REGISTRY } from 'videoforge';

const VALID_ANIMATION_TYPES = [
  'fade', 'slide', 'scale', 'rotate', 'bounce', 'pop', 'pulse', 'shake',
  'wobble', 'wave', 'swing', 'flip', 'typewriter', 'karaoke', 'reveal',
  'scramble', 'elastic', 'glitch', 'highlight', 'zoom', 'blurReveal', 'stagger',
];

const VALID_EFFECT_TYPES = [
  'glow', 'shadow', 'outline', 'gradient', 'neon', 'glass', 'blur', 'motionBlur',
  'backgroundBox', 'roundedBox', 'highlight', 'underline', 'strikethrough',
  'noise', 'grain', 'chromaticAberration', 'bloom', 'distortion', 'reflection',
];

const PRESET_DESCRIPTIONS = {
  hormozi:     'Bold all-caps Impact font, yellow highlights, aggressive outline, lower-third position',
  mrbeast:     'Large energetic text, yellow with black stroke, per-character pop animation',
  podcast:     'Clean sans-serif, word-level karaoke highlight, lower-third',
  news:        'Lower-third chyron, left-aligned text, red background bar',
  documentary: 'Elegant serif, centered, fade in/out, semi-transparent background',
  karaoke:     'Bold text, left-to-right fill progress, highlight bar',
  minimal:     'Light sans-serif, fade animation, pill-shaped background',
  gaming:      'Bold neon green, glitch animations, energetic style',
  luxury:      'Thin serif (Playfair Display), gold fill, wide tracking, blur-reveal animation',
  corporate:   'Professional clean style, fade + slide animation, dark background',
};

export class CaptionService {
  /**
   * @param {import('./ProjectService.js').ProjectService} projectService
   */
  constructor(projectService) {
    this._projects = projectService;
  }

  // ── Internal ──────────────────────────────────────────────────────────────────

  _requireCaptionClip(project, clipId) {
    for (const track of project.getTracks()) {
      const clip = track.getClip(clipId);
      if (clip !== undefined) {
        if (typeof clip.setTranscript !== 'function') {
          throw new Error(`Clip "${clipId}" is not a caption clip`);
        }
        return clip;
      }
    }
    throw new Error(`Clip not found: "${clipId}"`);
  }

  _styleSummary(style) {
    if (!style) return null;
    return {
      fontFamily:    style.fontFamily,
      fontSize:      style.fontSize,
      fontWeight:    style.fontWeight,
      fill:          style.fill,
      textAlign:     style.textAlign,
      verticalAlign: style.verticalAlign,
      stroke:        style.stroke   ? { color: style.stroke.color, width: style.stroke.width } : null,
      shadow:        style.shadow   ? { color: style.shadow.color, blur: style.shadow.blur }    : null,
      glow:          style.glow     ? { color: style.glow.color,   blur: style.glow.blur }      : null,
      background:    style.background ? { color: style.background.color } : null,
    };
  }

  _layoutSummary(layout) {
    if (!layout) return null;
    return {
      x:              layout.x,
      y:              layout.y,
      anchor:         layout.anchor,
      textAlign:      layout.textAlign,
      wrapMode:       layout.wrapMode,
      maxWordsPerLine: layout.maxWordsPerLine,
      safeZonePreset: layout.safeZonePreset,
      zIndex:         layout.zIndex,
    };
  }

  // ── Phase 4 — Core ────────────────────────────────────────────────────────────

  /**
   * Inspect a caption clip's full state.
   */
  inspectCaption(projectId, clipId) {
    const project = this._projects.getProject(projectId);
    const clip    = this._requireCaptionClip(project, clipId);
    const allWords = typeof clip.getAllWords === 'function' ? clip.getAllWords() : [];

    return {
      id:             clip.id,
      name:           clip.name,
      startTime:      clip.startTime,
      endTime:        clip.endTime,
      duration:       clip.duration,
      transcript:     clip.transcript,
      presetName:     clip.presetName ?? null,
      segmentCount:   clip.segments.length,
      wordCount:      allWords.length,
      animationCount: clip.captionAnimations?.length ?? 0,
      effectCount:    clip.captionEffects?.length ?? 0,
      hasKeyframes:   !(clip.captionKeyframeSet?.isEmpty?.() ?? true),
      style:          this._styleSummary(clip.style),
      layout:         this._layoutSummary(clip.layout),
      segments:       clip.segments.map((s, i) => ({
        index:     i,
        text:      s.text,
        timing:    s.timing,
        wordCount: s.words.length,
      })),
    };
  }

  /**
   * Set the caption transcript and auto-segment into lines.
   */
  setTranscript(projectId, clipId, transcript, options = {}) {
    const project = this._projects.getProject(projectId);
    const clip    = this._requireCaptionClip(project, clipId);

    clip.setTranscript(transcript, options);

    const allWords = typeof clip.getAllWords === 'function' ? clip.getAllWords() : [];
    return {
      clipId:       clip.id,
      transcript:   clip.transcript,
      segmentCount: clip.segments.length,
      wordCount:    allWords.length,
    };
  }

  /**
   * Apply one of the 10 built-in caption presets.
   */
  applyPreset(projectId, clipId, presetName) {
    const project = this._projects.getProject(projectId);
    const clip    = this._requireCaptionClip(project, clipId);

    clip.applyPreset(presetName);

    return {
      clipId:     clip.id,
      presetName: clip.presetName ?? presetName,
    };
  }

  /**
   * Set CaptionStyle properties on a caption clip.
   */
  setCaptionStyle(projectId, clipId, styleProps) {
    const project = this._projects.getProject(projectId);
    const clip    = this._requireCaptionClip(project, clipId);
    const s       = clip.style;

    // Typography
    if (styleProps.fontFamily    !== undefined) s.fontFamily    = styleProps.fontFamily;
    if (styleProps.fontSize      !== undefined) s.fontSize      = styleProps.fontSize;
    if (styleProps.fontWeight    !== undefined) s.fontWeight    = styleProps.fontWeight;
    if (styleProps.fontStyle     !== undefined) s.fontStyle     = styleProps.fontStyle;
    if (styleProps.textTransform !== undefined) s.textTransform = styleProps.textTransform;
    if (styleProps.letterSpacing !== undefined) s.letterSpacing = styleProps.letterSpacing;
    if (styleProps.lineHeight    !== undefined) s.lineHeight    = styleProps.lineHeight;
    if (styleProps.textAlign     !== undefined) s.textAlign     = styleProps.textAlign;
    if (styleProps.verticalAlign !== undefined) s.verticalAlign = styleProps.verticalAlign;
    if (styleProps.underline     !== undefined) s.underline     = styleProps.underline;
    if (styleProps.strikethrough !== undefined) s.strikethrough = styleProps.strikethrough;

    // Fill
    if (styleProps.fill        !== undefined) s.fill        = styleProps.fill;
    if (styleProps.fillOpacity !== undefined) s.fillOpacity = styleProps.fillOpacity;

    // Stroke
    if (styleProps.strokeColor !== undefined || styleProps.strokeWidth !== undefined) {
      s.setStroke(
        styleProps.strokeColor ?? s.stroke?.color ?? '#000000',
        styleProps.strokeWidth ?? s.stroke?.width ?? 2,
        styleProps.strokeJoin  ?? s.stroke?.join  ?? 'round',
      );
    }

    // Shadow
    if (styleProps.shadowColor !== undefined) {
      s.setShadow(
        styleProps.shadowColor,
        styleProps.shadowOffsetX ?? 2,
        styleProps.shadowOffsetY ?? 2,
        styleProps.shadowBlur    ?? 4,
      );
    }

    // Glow
    if (styleProps.glowColor !== undefined) {
      s.setGlow(
        styleProps.glowColor,
        styleProps.glowBlur     ?? 10,
        styleProps.glowStrength ?? 1,
      );
    }

    // Background
    if (styleProps.bgColor !== undefined) {
      s.setBackground(
        styleProps.bgColor,
        styleProps.bgPadding      ?? 8,
        styleProps.bgBorderRadius ?? 4,
        styleProps.bgOpacity      ?? 1,
      );
    }

    return { clipId: clip.id, style: this._styleSummary(s) };
  }

  /**
   * Set CaptionLayout properties on a caption clip.
   */
  setCaptionLayout(projectId, clipId, layoutProps) {
    const project = this._projects.getProject(projectId);
    const clip    = this._requireCaptionClip(project, clipId);
    const l       = clip.layout;

    if (layoutProps.x               !== undefined) l.x               = layoutProps.x;
    if (layoutProps.y               !== undefined) l.y               = layoutProps.y;
    if (layoutProps.anchor          !== undefined) l.anchor          = layoutProps.anchor;
    if (layoutProps.textAlign       !== undefined) l.textAlign       = layoutProps.textAlign;
    if (layoutProps.wrapMode        !== undefined) l.wrapMode        = layoutProps.wrapMode;
    if (layoutProps.maxWordsPerLine !== undefined) l.maxWordsPerLine = layoutProps.maxWordsPerLine;
    if (layoutProps.maxCharsPerLine !== undefined) l.maxCharsPerLine = layoutProps.maxCharsPerLine;
    if (layoutProps.maxWidth        !== undefined) l.maxWidth        = layoutProps.maxWidth;
    if (layoutProps.maxHeight       !== undefined) l.maxHeight       = layoutProps.maxHeight;
    if (layoutProps.padding         !== undefined) l.padding         = layoutProps.padding;
    if (layoutProps.zIndex          !== undefined) l.zIndex          = layoutProps.zIndex;

    if (layoutProps.safeZonePreset !== undefined) {
      l.applySafeZone(layoutProps.safeZonePreset);
    }

    return { clipId: clip.id, layout: this._layoutSummary(l) };
  }

  /**
   * Return the 10 built-in preset names and descriptions.
   */
  listPresets() {
    const presets = Object.entries(PRESET_DESCRIPTIONS).map(([name, description]) => ({
      name,
      description,
    }));
    return { count: presets.length, presets };
  }

  /**
   * Return the words active at a given absolute timeline time.
   */
  getActiveWords(projectId, clipId, time) {
    const project = this._projects.getProject(projectId);
    const clip    = this._requireCaptionClip(project, clipId);
    const words   = clip.getActiveWords(time);

    return {
      clipId:          clip.id,
      time,
      activeWordCount: words.length,
      words: words.map((w) => ({
        index:       w.index,
        text:        w.text,
        timing:      w.timing,
        highlighted: w.highlighted,
      })),
    };
  }

  /**
   * Add a caption animation at the specified target level.
   * target: 'caption' | 'lines' | 'words' | 'characters'
   */
  addAnimation(projectId, clipId, animationType, target = 'caption', options = {}) {
    const project = this._projects.getProject(projectId);
    const clip    = this._requireCaptionClip(project, clipId);

    if (!VALID_ANIMATION_TYPES.includes(animationType)) {
      throw new Error(
        `Unknown animation type "${animationType}". Valid: ${VALID_ANIMATION_TYPES.join(', ')}`
      );
    }

    switch (target) {
      case 'caption':                clip.animateCaption(animationType, options);    break;
      case 'lines': case 'segments': clip.animateLines(animationType, options);      break;
      case 'words':                  clip.animateWords(animationType, options);      break;
      case 'characters':             clip.animateCharacters(animationType, options); break;
      default:
        throw new Error(`Invalid target "${target}". Must be: caption, lines, words, characters`);
    }

    return {
      clipId:         clip.id,
      animationType,
      target,
      totalAnimations: clip.captionAnimations?.length ?? 0,
    };
  }

  // ── Phase 5 — Advanced ────────────────────────────────────────────────────────

  /**
   * Add a CaptionEffect to a caption clip.
   */
  addEffect(projectId, clipId, effectType, params = {}) {
    const project = this._projects.getProject(projectId);
    const clip    = this._requireCaptionClip(project, clipId);

    const EffectClass = EFFECT_REGISTRY.get(effectType);
    if (!EffectClass) {
      throw new Error(
        `Unknown effect type "${effectType}". Valid: ${VALID_EFFECT_TYPES.join(', ')}`
      );
    }

    const effect = new EffectClass(params);
    clip.addEffect(effect);

    return {
      clipId:       clip.id,
      effectId:     effect.id,
      effectType,
      totalEffects: clip.captionEffects?.length ?? 0,
    };
  }

  /**
   * Remove a caption animation by ID.
   */
  removeAnimation(projectId, clipId, animationId) {
    const project = this._projects.getProject(projectId);
    const clip    = this._requireCaptionClip(project, clipId);

    const removed = clip.removeAnimation(animationId);
    if (!removed) throw new Error(`Animation not found: "${animationId}"`);

    return { clipId: clip.id, removed: true, totalAnimations: clip.captionAnimations?.length ?? 0 };
  }

  /**
   * Remove a caption effect by ID.
   */
  removeEffect(projectId, clipId, effectId) {
    const project = this._projects.getProject(projectId);
    const clip    = this._requireCaptionClip(project, clipId);

    const removed = clip.removeEffect(effectId);
    if (!removed) throw new Error(`Effect not found: "${effectId}"`);

    return { clipId: clip.id, removed: true, totalEffects: clip.captionEffects?.length ?? 0 };
  }

  /**
   * Add a keyframe to the caption clip's keyframe set.
   */
  addKeyframe(projectId, clipId, property, time, value, easing = 'linear') {
    const project = this._projects.getProject(projectId);
    const clip    = this._requireCaptionClip(project, clipId);

    clip.addKeyframe(property, time, value, easing);

    return {
      clipId:      clip.id,
      property,
      time,
      value,
      easing,
      hasKeyframes: !(clip.captionKeyframeSet?.isEmpty?.() ?? true),
    };
  }

  /**
   * Highlight a word by flat global index across all segments.
   */
  highlightWord(projectId, clipId, wordIndex, styleOverride = {}) {
    const project = this._projects.getProject(projectId);
    const clip    = this._requireCaptionClip(project, clipId);

    clip.highlightWord(wordIndex, styleOverride);
    return { clipId: clip.id, wordIndex, highlighted: true };
  }

  /**
   * Highlight all words matching any of the given keywords.
   */
  highlightKeywords(projectId, clipId, keywords, styleOverride = {}) {
    const project = this._projects.getProject(projectId);
    const clip    = this._requireCaptionClip(project, clipId);

    clip.highlightKeywords(keywords, styleOverride);
    return { clipId: clip.id, keywords, highlighted: true };
  }

  /**
   * Clear all word highlights on the caption clip.
   */
  clearHighlights(projectId, clipId) {
    const project = this._projects.getProject(projectId);
    const clip    = this._requireCaptionClip(project, clipId);

    clip.clearHighlights();
    return { clipId: clip.id, cleared: true };
  }

  /**
   * Configure the caption clip for karaoke playback.
   */
  buildKaraoke(projectId, clipId, options = {}) {
    const project = this._projects.getProject(projectId);
    const clip    = this._requireCaptionClip(project, clipId);

    clip.buildKaraoke(options);
    return { clipId: clip.id, karaokeEnabled: true, options };
  }

  /**
   * Apply an animation to all words across all segments.
   */
  animateWords(projectId, clipId, animationType, options = {}) {
    const project = this._projects.getProject(projectId);
    const clip    = this._requireCaptionClip(project, clipId);

    if (!VALID_ANIMATION_TYPES.includes(animationType)) {
      throw new Error(
        `Unknown animation type "${animationType}". Valid: ${VALID_ANIMATION_TYPES.join(', ')}`
      );
    }

    clip.animateWords(animationType, options);
    return { clipId: clip.id, animationType, target: 'words' };
  }

  /**
   * Apply an animation to all characters across all segments.
   */
  animateCharacters(projectId, clipId, animationType, options = {}) {
    const project = this._projects.getProject(projectId);
    const clip    = this._requireCaptionClip(project, clipId);

    if (!VALID_ANIMATION_TYPES.includes(animationType)) {
      throw new Error(
        `Unknown animation type "${animationType}". Valid: ${VALID_ANIMATION_TYPES.join(', ')}`
      );
    }

    clip.animateCharacters(animationType, options);
    return { clipId: clip.id, animationType, target: 'characters' };
  }

  /**
   * Override the style on a specific segment by index.
   */
  setSegmentStyle(projectId, clipId, segmentIndex, styleProps) {
    const project = this._projects.getProject(projectId);
    const clip    = this._requireCaptionClip(project, clipId);

    if (segmentIndex < 0 || segmentIndex >= clip.segments.length) {
      throw new Error(
        `Segment index ${segmentIndex} out of range (0–${clip.segments.length - 1})`
      );
    }

    const segment = clip.segments[segmentIndex];
    if (segment.style && typeof segment.style === 'object') {
      Object.assign(segment.style, styleProps);
    } else {
      segment.style = { ...styleProps };
    }

    return {
      clipId:       clip.id,
      segmentIndex,
      segmentText:  segment.text,
    };
  }
}
