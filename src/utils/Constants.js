/**
 * @module Constants
 * Shared enumerations and defaults for VideoForge.
 */

/** Track types supported by the engine. */
export const TRACK_TYPES = Object.freeze({
  VIDEO: 'video',
  AUDIO: 'audio',
  IMAGE: 'image',
  TEXT: 'text',
  SHAPE: 'shape',
});

/** Clip types — mirrors the concrete Clip subclasses. */
export const CLIP_TYPES = Object.freeze({
  VIDEO: 'video',
  AUDIO: 'audio',
  IMAGE: 'image',
  TEXT: 'text',
  SHAPE: 'shape',
});

/** Asset media types. */
export const ASSET_TYPES = Object.freeze({
  VIDEO: 'video',
  AUDIO: 'audio',
  IMAGE: 'image',
  FONT: 'font',
  SYNTHETIC: 'synthetic', // text, shapes — no file on disk
});

/** Effect types. */
export const EFFECT_TYPES = Object.freeze({
  FADE_IN: 'fadeIn',
  FADE_OUT: 'fadeOut',
  TRANSITION: 'transition',
  COLOR_CORRECTION: 'colorCorrection',
  BLUR: 'blur',
  CROP: 'crop',
  CHROMA_KEY: 'chromaKey',
  CUSTOM: 'custom',
});

/** Alignment options for the crop effect. */
export const CROP_ALIGNMENT = Object.freeze({
  CENTER:       'center',
  TOP:          'top',
  BOTTOM:       'bottom',
  LEFT:         'left',
  RIGHT:        'right',
  TOP_LEFT:     'topLeft',
  TOP_RIGHT:    'topRight',
  BOTTOM_LEFT:  'bottomLeft',
  BOTTOM_RIGHT: 'bottomRight',
});

/** Transition sub-types. */
export const TRANSITION_TYPES = Object.freeze({
  CROSS_DISSOLVE: 'crossDissolve',
  WIPE_LEFT: 'wipeLeft',
  WIPE_RIGHT: 'wipeRight',
  WIPE_UP: 'wipeUp',
  WIPE_DOWN: 'wipeDown',
  SLIDE: 'slide',
  ZOOM: 'zoom',
  DIP_TO_BLACK: 'dipToBlack',
  DIP_TO_WHITE: 'dipToWhite',
});

/** Easing functions (used in effects). */
export const EASING = Object.freeze({
  LINEAR: 'linear',
  EASE_IN: 'easeIn',
  EASE_OUT: 'easeOut',
  EASE_IN_OUT: 'easeInOut',
});

/** Export format types. */
export const EXPORT_TYPES = Object.freeze({
  JSON: 'json',
  PREMIERE: 'premiere',
  FCPXML: 'fcpxml',
  EDL: 'edl',
  MP4: 'mp4',
});

/** Text alignment options. */
export const TEXT_ALIGN = Object.freeze({
  LEFT: 'left',
  CENTER: 'center',
  RIGHT: 'right',
  JUSTIFY: 'justify',
});

/** Shape types. */
export const SHAPE_TYPES = Object.freeze({
  RECTANGLE: 'rectangle',
  ELLIPSE: 'ellipse',
  TRIANGLE: 'triangle',
  LINE: 'line',
  POLYGON: 'polygon',
  ARROW: 'arrow',
});

/** Preview player states. */
export const PLAYER_STATE = Object.freeze({
  IDLE: 'idle',
  PLAYING: 'playing',
  PAUSED: 'paused',
  BUFFERING: 'buffering',
  ENDED: 'ended',
});

/** Default project settings. */
export const DEFAULTS = Object.freeze({
  FPS: 30,
  WIDTH: 1920,
  HEIGHT: 1080,
  SAMPLE_RATE: 48000,
  BIT_DEPTH: 16,
  CHANNELS: 2,
  VIDEO_BITRATE: '8000k',
  AUDIO_BITRATE: '192k',
});
