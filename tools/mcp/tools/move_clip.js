/**
 * @module tools/move_clip
 * MCP tool: move a clip to a new timeline start position.
 */

import { InputValidator } from '../validators/InputValidator.js';

export const definition = {
  name: 'move_clip',
  description:
    'Move a clip to a new timeline start position (in seconds). ' +
    'The clip duration does not change, only its placement on the timeline.',
  inputSchema: {
    type: 'object',
    properties: {
      projectId: { type: 'string', description: 'Project ID.' },
      clipId:    { type: 'string', description: 'Clip ID to move.' },
      startTime: { type: 'number', minimum: 0, description: 'New timeline start position in seconds.' },
    },
    required: ['projectId', 'clipId', 'startTime'],
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
    startTime: { type: 'number', required: true },
  });

  return clipEditingService.moveClip(args.projectId, args.clipId, args.startTime);
}
