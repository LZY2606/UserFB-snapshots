/**
 * @module tools/add_crop_effect
 * MCP tool: crop pixels from one or more edges of a clip.
 */

import { InputValidator } from '../validators/InputValidator.js';

const ALIGNMENT_VALUES = [
  'center', 'top', 'bottom', 'left', 'right',
  'topLeft', 'topRight', 'bottomLeft', 'bottomRight',
];

export const definition = {
  name: 'add_crop_effect',
  description:
    'Crop pixels from one or more edges of a clip. ' +
    'Specify the number of pixels to remove from each edge (top, bottom, left, right). ' +
    'The alignment controls where the cropped content is placed within the original canvas. ' +
    'Works on video, image, and text clips. Returns the crop effect ID for later removal.',
  inputSchema: {
    type: 'object',
    properties: {
      projectId: { type: 'string', description: 'Project ID.' },
      clipId:    { type: 'string', description: 'Clip ID to crop.' },
      top:    { type: 'number', minimum: 0, description: 'Pixels to remove from the top edge. Default: 0.' },
      bottom: { type: 'number', minimum: 0, description: 'Pixels to remove from the bottom edge. Default: 0.' },
      left:   { type: 'number', minimum: 0, description: 'Pixels to remove from the left edge. Default: 0.' },
      right:  { type: 'number', minimum: 0, description: 'Pixels to remove from the right edge. Default: 0.' },
      alignment: {
        type: 'string',
        enum: ALIGNMENT_VALUES,
        description:
          'Where to place the cropped content within the original canvas. ' +
          'Default: "center". Options: center, top, bottom, left, right, ' +
          'topLeft, topRight, bottomLeft, bottomRight.',
      },
    },
    required: ['projectId', 'clipId'],
  },
};

/**
 * @param {object} args
 * @param {{ clipEditingService: import('../services/ClipEditingService.js').ClipEditingService }} services
 */
export function handler(args, { clipEditingService }) {
  InputValidator.assert(args, {
    projectId: { type: 'string', required: true, minLength: 1 },
    clipId:    { type: 'string', required: true, minLength: 1 },
  });

  const result = clipEditingService.addCrop(args.projectId, args.clipId, {
    top:       args.top,
    bottom:    args.bottom,
    left:      args.left,
    right:     args.right,
    alignment: args.alignment,
  });

  return {
    ...result,
    message:
      `Crop effect (id: ${result.effectId}) added to clip "${result.clipId}" — ` +
      `top:${result.params.top} bottom:${result.params.bottom} ` +
      `left:${result.params.left} right:${result.params.right} ` +
      `alignment:${result.params.alignment}.`,
  };
}
