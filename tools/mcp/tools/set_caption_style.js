/**
 * @module tools/set_caption_style
 * MCP tool: set CaptionStyle properties on a caption clip.
 */

import { InputValidator } from '../validators/InputValidator.js';

export const definition = {
  name: 'set_caption_style',
  description:
    'Set typography and visual style properties on a caption clip. ' +
    'All properties are optional — only supplied fields are updated. ' +
    'For stroke, shadow, glow, and background provide the primary color; ' +
    'fine-tune with the extended fields (e.g. strokeWidth, shadowBlur, glowStrength, bgPadding).',
  inputSchema: {
    type: 'object',
    properties: {
      projectId: { type: 'string', description: 'Project ID.' },
      clipId:    { type: 'string', description: 'Caption clip ID.' },

      // Typography
      fontFamily:    { type: 'string',  description: 'Font family (e.g. "Arial", "Impact").' },
      fontSize:      { type: 'number',  minimum: 1,  description: 'Font size in pixels.' },
      fontWeight:    { description: 'Font weight: 100–900 or "bold"/"normal".' },
      fontStyle:     { type: 'string',  enum: ['normal', 'italic', 'oblique'], description: 'Font style.' },
      textTransform: { type: 'string',  enum: ['none', 'uppercase', 'lowercase', 'capitalize'], description: 'Text transform.' },
      letterSpacing: { type: 'number',  description: 'Letter spacing in pixels.' },
      lineHeight:    { type: 'number',  minimum: 0,  description: 'Line height multiplier (e.g. 1.2).' },
      textAlign:     { type: 'string',  enum: ['left', 'center', 'right', 'justify'], description: 'Horizontal text alignment.' },
      verticalAlign: { type: 'string',  enum: ['top', 'middle', 'bottom'], description: 'Vertical alignment.' },

      // Fill
      fill:        { type: 'string',  description: 'Text fill color (hex or CSS color, e.g. "#FFFFFF").' },
      fillOpacity: { type: 'number',  minimum: 0, maximum: 1, description: 'Fill opacity (0–1).' },

      // Stroke
      strokeColor: { type: 'string',  description: 'Stroke/outline color.' },
      strokeWidth: { type: 'number',  minimum: 0, description: 'Stroke width in pixels.' },
      strokeJoin:  { type: 'string',  enum: ['miter', 'round', 'bevel'], description: 'Stroke join style.' },

      // Shadow
      shadowColor:   { type: 'string', description: 'Drop shadow color.' },
      shadowOffsetX: { type: 'number', description: 'Shadow X offset in pixels.' },
      shadowOffsetY: { type: 'number', description: 'Shadow Y offset in pixels.' },
      shadowBlur:    { type: 'number', minimum: 0, description: 'Shadow blur radius.' },

      // Glow
      glowColor:    { type: 'string', description: 'Glow color.' },
      glowBlur:     { type: 'number', minimum: 0, description: 'Glow blur radius.' },
      glowStrength: { type: 'number', minimum: 0, description: 'Glow strength multiplier.' },

      // Background
      bgColor:        { type: 'string', description: 'Background box color.' },
      bgPadding:      { type: 'number', minimum: 0, description: 'Background padding in pixels.' },
      bgBorderRadius: { type: 'number', minimum: 0, description: 'Background corner radius in pixels.' },
      bgOpacity:      { type: 'number', minimum: 0, maximum: 1, description: 'Background opacity (0–1).' },

      // Decorations
      underline:     { type: 'boolean', description: 'Enable underline.' },
      strikethrough: { type: 'boolean', description: 'Enable strikethrough.' },
    },
    required: ['projectId', 'clipId'],
  },
};

export function handler(args, { captionService }) {
  InputValidator.assert(args, {
    projectId: { type: 'string', required: true, minLength: 1 },
    clipId:    { type: 'string', required: true, minLength: 1 },
  });

  const styleProps = {};
  const fields = [
    'fontFamily', 'fontSize', 'fontWeight', 'fontStyle', 'textTransform',
    'letterSpacing', 'lineHeight', 'textAlign', 'verticalAlign',
    'fill', 'fillOpacity',
    'strokeColor', 'strokeWidth', 'strokeJoin',
    'shadowColor', 'shadowOffsetX', 'shadowOffsetY', 'shadowBlur',
    'glowColor', 'glowBlur', 'glowStrength',
    'bgColor', 'bgPadding', 'bgBorderRadius', 'bgOpacity',
    'underline', 'strikethrough',
  ];
  for (const f of fields) {
    if (args[f] !== undefined) styleProps[f] = args[f];
  }

  return captionService.setCaptionStyle(args.projectId, args.clipId, styleProps);
}
