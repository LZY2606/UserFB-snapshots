/**
 * @module tools/set_image_transform
 * MCP tool: set image clip transform properties.
 */

import { InputValidator } from '../validators/InputValidator.js';

export const definition = {
  name: 'set_image_transform',
  description:
    'Set transform properties on an image clip: position (x, y), scale (scaleX, scaleY), ' +
    'rotation (degrees), and opacity (0–1).',
  inputSchema: {
    type: 'object',
    properties: {
      projectId:    { type: 'string',  description: 'Project ID.' },
      clipId:       { type: 'string',  description: 'Image clip ID.' },
      x:            { type: 'number',  description: 'Canvas X position in pixels.' },
      y:            { type: 'number',  description: 'Canvas Y position in pixels.' },
      scaleX:       { type: 'number',  minimum: 0, description: 'Horizontal scale factor. 1 = original size.' },
      scaleY:       { type: 'number',  minimum: 0, description: 'Vertical scale factor. 1 = original size.' },
      rotation:     { type: 'number',  description: 'Rotation in degrees (clockwise).' },
      opacityLevel: { type: 'number',  minimum: 0, maximum: 1, description: 'Opacity (0 = transparent, 1 = opaque).' },
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

  return clipStyleService.setImageTransform(args.projectId, args.clipId, {
    x:            args.x,
    y:            args.y,
    scaleX:       args.scaleX,
    scaleY:       args.scaleY,
    rotation:     args.rotation,
    opacityLevel: args.opacityLevel,
  });
}
