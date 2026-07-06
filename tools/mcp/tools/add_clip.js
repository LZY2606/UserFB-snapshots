/**
 * @module tools/add_clip
 * MCP tool: unified clip factory — adds any clip type to a track.
 *
 * The `src` field meaning changes per clipType:
 *   video / audio / image → absolute file path
 *   text / caption        → text or transcript content
 *   shape                 → shape type (rectangle | ellipse | triangle | line | polygon | arrow)
 */

import { InputValidator } from '../validators/InputValidator.js';

export const definition = {
  name: 'add_clip',
  description:
    'Add a clip to a track. Supports all six clip types: video, audio, image, text, shape, caption. ' +
    'Returns the new clipId and timing info.',
  inputSchema: {
    type: 'object',
    properties: {
      projectId: { type: 'string', description: 'Project ID.' },
      trackId:   { type: 'string', description: 'Track ID to add the clip to.' },
      clipType: {
        type: 'string',
        description: '"video" | "audio" | "image" | "text" | "shape" | "caption".',
        enum: ['video', 'audio', 'image', 'text', 'shape', 'caption'],
      },
      src: {
        type: 'string',
        description:
          'File path (video/audio/image), text content (text/caption), ' +
          'or shape type — rectangle, ellipse, triangle, line, polygon, arrow (shape).',
      },

      // ── Common clip options ─────────────────────────────────────────────────
      name:      { type: 'string', description: 'Clip display name.' },
      startTime: { type: 'number', description: 'Timeline start time in seconds (default: end of existing clips).' },
      inPoint:   { type: 'number', description: 'Source in-point in seconds.' },
      outPoint:  { type: 'number', description: 'Source out-point in seconds.' },

      // ── Video options ───────────────────────────────────────────────────────
      volumeLevel:  { type: 'number',  description: '(video/audio) Volume 0–2.' },
      playbackRate: { type: 'number',  description: '(video/audio) Playback speed multiplier.' },
      reversed:     { type: 'boolean', description: '(video) Play clip in reverse.' },

      // ── Audio options ───────────────────────────────────────────────────────
      panValue: { type: 'number', description: '(audio) Stereo pan -1 (left) to +1 (right).' },

      // ── Image / transform options ───────────────────────────────────────────
      x:            { type: 'number', description: '(image/text/shape) X position in pixels.' },
      y:            { type: 'number', description: '(image/text/shape) Y position in pixels.' },
      scaleX:       { type: 'number', description: '(image) Horizontal scale factor.' },
      scaleY:       { type: 'number', description: '(image) Vertical scale factor.' },
      rotation:     { type: 'number', description: '(image) Rotation in degrees.' },
      opacityLevel: { type: 'number', description: '(image/text/shape) Opacity 0–1.' },

      // ── Text options ────────────────────────────────────────────────────────
      fontFamily:    { type: 'string',  description: '(text) Font family name.' },
      fontSizeValue: { type: 'number',  description: '(text) Font size in px.' },
      colorValue:    { type: 'string',  description: '(text) Text colour hex string.' },
      bgColor:       { type: 'string',  description: '(text) Background colour hex string.' },
      alignValue:    { type: 'string',  description: '(text) Text align: left | center | right | justify.', enum: ['left', 'center', 'right', 'justify'] },
      bold:          { type: 'boolean', description: '(text) Bold.' },
      italic:        { type: 'boolean', description: '(text) Italic.' },

      // ── Shape options ───────────────────────────────────────────────────────
      width:            { type: 'number', description: '(shape) Width in pixels.' },
      height:           { type: 'number', description: '(shape) Height in pixels.' },
      fillColor:        { type: 'string', description: '(shape) Fill colour hex string.' },
      strokeColorValue: { type: 'string', description: '(shape) Stroke colour hex string.' },
      strokeWidthValue: { type: 'number', description: '(shape) Stroke width in pixels.' },
      rotationDeg:      { type: 'number', description: '(shape) Rotation in degrees.' },
      cornerRadius:     { type: 'number', description: '(shape/rectangle) Corner radius in pixels.' },

      // ── Caption options ─────────────────────────────────────────────────────
      preset:             { type: 'string', description: '(caption) Preset name to apply immediately.' },
      maxWordsPerSegment: { type: 'number', description: '(caption) Max words per auto-segment.' },
    },
    required: ['projectId', 'trackId', 'clipType', 'src'],
  },
};

/**
 * @param {object} args
 * @param {{ clipService: import('../services/ClipService.js').ClipService }} services
 */
export function handler(args, { clipService }) {
  InputValidator.assert(args, {
    projectId: { type: 'string', required: true, minLength: 1 },
    trackId:   { type: 'string', required: true, minLength: 1 },
    clipType:  { type: 'string', required: true, enum: ['video', 'audio', 'image', 'text', 'shape', 'caption'] },
    src:       { type: 'string', required: true, minLength: 1 },
    volumeLevel:  { type: 'number', min: 0,  max: 2   },
    panValue:     { type: 'number', min: -1, max: 1   },
    opacityLevel: { type: 'number', min: 0,  max: 1   },
    playbackRate: { type: 'number', min: 0.1, max: 16 },
  });

  const { projectId, trackId, clipType, src, ...opts } = args;
  const result = clipService.addClip(projectId, trackId, clipType, src, opts);

  return {
    ...result,
    message:
      `${clipType} clip "${result.name}" added to track "${trackId}". ` +
      `clipId: "${result.clipId}" | start: ${result.startTime}s | duration: ${result.duration}s.`,
  };
}
