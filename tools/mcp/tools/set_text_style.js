/**
 * @module tools/set_text_style
 * MCP tool: set text clip style properties.
 */

import { InputValidator } from '../validators/InputValidator.js';

export const definition = {
  name: 'set_text_style',
  description:
    'Set style properties on a text clip: font family, font size, color, background color, ' +
    'text alignment (left|center|right|justify), bold, italic, position (x, y), opacity.',
  inputSchema: {
    type: 'object',
    properties: {
      projectId:     { type: 'string',  description: 'Project ID.' },
      clipId:        { type: 'string',  description: 'Text clip ID.' },
      fontFamily:    { type: 'string',  description: 'Font family name (e.g. "Arial").' },
      fontSizeValue: { type: 'number',  minimum: 1, description: 'Font size in pixels.' },
      colorValue:    { type: 'string',  description: 'Text color (hex or CSS color, e.g. "#FFFFFF").' },
      bgColor:       { type: 'string',  description: 'Background color (hex, CSS color, or "transparent").' },
      alignValue: {
        type: 'string',
        enum: ['left', 'center', 'right', 'justify'],
        description: 'Text alignment.',
      },
      bold:         { type: 'boolean', description: 'Bold text.' },
      italic:       { type: 'boolean', description: 'Italic text.' },
      x:            { type: 'number',  description: 'Canvas X position in pixels.' },
      y:            { type: 'number',  description: 'Canvas Y position in pixels.' },
      opacityLevel: { type: 'number',  minimum: 0, maximum: 1, description: 'Opacity (0–1).' },
    },
    required: ['projectId', 'clipId'],
  },
};

/**
 * @param {object} args
 * @param {{ clipStyleService: import('../services/ClipStyleService.js').ClipStyleService }} services
 */
export function handler(args, { clipStyleService }) {
  InputValidator.assert(args, {
    projectId: { type: 'string', required: true, minLength: 1 },
    clipId:    { type: 'string', required: true, minLength: 1 },
  });

  return clipStyleService.setTextStyle(args.projectId, args.clipId, {
    fontFamily:    args.fontFamily,
    fontSizeValue: args.fontSizeValue,
    colorValue:    args.colorValue,
    bgColor:       args.bgColor,
    alignValue:    args.alignValue,
    bold:          args.bold,
    italic:        args.italic,
    x:             args.x,
    y:             args.y,
    opacityLevel:  args.opacityLevel,
  });
}
