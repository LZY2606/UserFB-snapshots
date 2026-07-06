/**
 * @module tools/split_clip
 * MCP tool: split a clip at an absolute timeline time.
 */

import { InputValidator } from '../validators/InputValidator.js';

export const definition = {
  name: 'split_clip',
  description:
    'Split a clip at an absolute timeline time (in seconds). ' +
    'Returns the IDs and timing of the resulting head and tail clips.',
  inputSchema: {
    type: 'object',
    properties: {
      projectId: { type: 'string', description: 'Project ID.' },
      clipId:    { type: 'string', description: 'Clip ID to split.' },
      splitTime: {
        type: 'number',
        minimum: 0,
        description: 'Absolute timeline time (seconds) at which to split the clip.',
      },
    },
    required: ['projectId', 'clipId', 'splitTime'],
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
    splitTime: { type: 'number', required: true },
  });

  return clipEditingService.splitClip(args.projectId, args.clipId, args.splitTime);
}
