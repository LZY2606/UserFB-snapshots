/**
 * @module tools/copy_clip
 * MCP tool: deep-copy a clip, optionally repositioning the new copy.
 */

import { InputValidator } from '../validators/InputValidator.js';

export const definition = {
  name: 'copy_clip',
  description:
    'Create a deep copy of a clip (new ID, cloned effects). ' +
    'Optionally reposition the copy on the timeline via startTime.',
  inputSchema: {
    type: 'object',
    properties: {
      projectId: { type: 'string', description: 'Project ID.' },
      clipId:    { type: 'string', description: 'Clip ID to copy.' },
      startTime: {
        type: 'number',
        minimum: 0,
        description: 'Timeline start position for the copy (seconds). Omit to keep same position as original.',
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

  return clipEditingService.copyClip(args.projectId, args.clipId, args.startTime);
}
