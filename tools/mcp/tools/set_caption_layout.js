/**
 * @module tools/set_caption_layout
 * MCP tool: set CaptionLayout (position, wrapping, safe zones) on a caption clip.
 */

import { InputValidator } from '../validators/InputValidator.js';

const SAFE_ZONE_PRESETS = [
  'tiktok', 'instagram', 'youtube', 'shorts', 'reels',
  'twitter', 'broadcast', 'action', 'title',
];

const ANCHOR_POINTS = [
  'topLeft', 'topCenter', 'topRight',
  'middleLeft', 'center', 'middleRight',
  'bottomLeft', 'bottomCenter', 'bottomRight',
];

export const definition = {
  name: 'set_caption_layout',
  description:
    'Set the spatial layout for a caption clip: position (x, y as 0–1 fractions of canvas), ' +
    'anchor point, safe zone preset, text alignment, wrapping mode, and line width limits.',
  inputSchema: {
    type: 'object',
    properties: {
      projectId: { type: 'string', description: 'Project ID.' },
      clipId:    { type: 'string', description: 'Caption clip ID.' },

      x: { type: 'number', minimum: 0, maximum: 1, description: 'Normalized X position (0 = left, 1 = right). Default: 0.5 (center).' },
      y: { type: 'number', minimum: 0, maximum: 1, description: 'Normalized Y position (0 = top, 1 = bottom). Default: 0.85 (lower-third).' },
      anchor: {
        type: 'string',
        enum: ANCHOR_POINTS,
        description: 'Anchor point on the caption box relative to x/y. Default: bottomCenter.',
      },
      safeZonePreset: {
        type: 'string',
        enum: SAFE_ZONE_PRESETS,
        description: 'Apply a platform safe zone: tiktok, instagram, youtube, shorts, reels, twitter, broadcast, action, title.',
      },
      textAlign: {
        type: 'string',
        enum: ['left', 'center', 'right', 'justify'],
        description: 'Text alignment within the caption box.',
      },
      wrapMode: {
        type: 'string',
        enum: ['none', 'word', 'character', 'auto'],
        description: 'Word-wrap mode. Default: word.',
      },
      maxWordsPerLine: { type: 'number', minimum: 1, description: 'Maximum words per display line.' },
      maxCharsPerLine: { type: 'number', minimum: 1, description: 'Maximum characters per display line.' },
      maxWidth:  { type: 'number', minimum: 0, description: 'Maximum caption box width in pixels.' },
      maxHeight: { type: 'number', minimum: 0, description: 'Maximum caption box height in pixels.' },
      padding:   { type: 'number', minimum: 0, description: 'Inner padding in pixels.' },
      zIndex:    { type: 'number', description: 'Z-order (higher = in front).' },
    },
    required: ['projectId', 'clipId'],
  },
};

export function handler(args, { captionService }) {
  InputValidator.assert(args, {
    projectId: { type: 'string', required: true, minLength: 1 },
    clipId:    { type: 'string', required: true, minLength: 1 },
  });

  const layoutProps = {};
  const fields = [
    'x', 'y', 'anchor', 'safeZonePreset', 'textAlign', 'wrapMode',
    'maxWordsPerLine', 'maxCharsPerLine', 'maxWidth', 'maxHeight', 'padding', 'zIndex',
  ];
  for (const f of fields) {
    if (args[f] !== undefined) layoutProps[f] = args[f];
  }

  return captionService.setCaptionLayout(args.projectId, args.clipId, layoutProps);
}
