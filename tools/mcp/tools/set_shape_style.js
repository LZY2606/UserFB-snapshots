/**
 * @module tools/set_shape_style
 * MCP tool: set shape clip style properties.
 */

import { InputValidator } from '../validators/InputValidator.js';

export const definition = {
  name: 'set_shape_style',
  description:
    'Set style properties on a shape clip: position (x, y), size (width, height), ' +
    'fill color, stroke color, stroke width, rotation, corner radius, opacity.',
  inputSchema: {
    type: 'object',
    properties: {
      projectId:        { type: 'string', description: 'Project ID.' },
      clipId:           { type: 'string', description: 'Shape clip ID.' },
      x:                { type: 'number', description: 'Canvas X position in pixels.' },
      y:                { type: 'number', description: 'Canvas Y position in pixels.' },
      width:            { type: 'number', minimum: 0, description: 'Shape width in pixels.' },
      height:           { type: 'number', minimum: 0, description: 'Shape height in pixels.' },
      fillColor:        { type: 'string', description: 'Fill color (hex or CSS color).' },
      strokeColorValue: { type: 'string', description: 'Stroke color (hex or CSS color, or "transparent").' },
      strokeWidthValue: { type: 'number', minimum: 0, description: 'Stroke width in pixels.' },
      rotationDeg:      { type: 'number', description: 'Rotation in degrees.' },
      cornerRadius:     { type: 'number', minimum: 0, description: 'Corner radius in pixels (for rectangles).' },
      opacityLevel:     { type: 'number', minimum: 0, maximum: 1, description: 'Opacity (0–1).' },
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

  return clipStyleService.setShapeStyle(args.projectId, args.clipId, {
    x:                args.x,
    y:                args.y,
    width:            args.width,
    height:           args.height,
    fillColor:        args.fillColor,
    strokeColorValue: args.strokeColorValue,
    strokeWidthValue: args.strokeWidthValue,
    rotationDeg:      args.rotationDeg,
    cornerRadius:     args.cornerRadius,
    opacityLevel:     args.opacityLevel,
  });
}
