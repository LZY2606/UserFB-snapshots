/**
 * @module tools/trim_clip
 * MCP tool: trim a clip's source in/out points.
 */

import { InputValidator } from '../validators/InputValidator.js';

export const definition = {
  name: 'trim_clip',
  description:
    'Trim a clip by setting its source in-point and out-point (in seconds). ' +
    'This changes how much of the source media is used, not the timeline position.',
  inputSchema: {
    type: 'object',
    properties: {
      projectId: { type: 'string', description: 'Project ID.' },
      clipId:    { type: 'string', description: 'Clip ID to trim.' },
      inPoint:   { type: 'number', minimum: 0, description: 'Source in-point in seconds.' },
      outPoint:  { type: 'number', minimum: 0, description: 'Source out-point in seconds.' },
    },
    required: ['projectId', 'clipId', 'inPoint', 'outPoint'],
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
    inPoint:   { type: 'number', required: true },
    outPoint:  { type: 'number', required: true },
  });

  return clipEditingService.trimClip(args.projectId, args.clipId, args.inPoint, args.outPoint);
}
