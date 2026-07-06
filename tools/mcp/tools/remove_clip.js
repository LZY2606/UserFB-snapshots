/**
 * @module tools/remove_clip
 * MCP tool: remove a clip from its track by clip ID.
 */

import { InputValidator } from '../validators/InputValidator.js';

export const definition = {
  name: 'remove_clip',
  description: 'Remove a clip from its track. Searches all tracks in the project.',
  inputSchema: {
    type: 'object',
    properties: {
      projectId: { type: 'string', description: 'Project ID.' },
      clipId:    { type: 'string', description: 'Clip ID to remove.' },
    },
    required: ['projectId', 'clipId'],
  },
};

/**
 * @param {object} args
 * @param {{ clipService: import('../services/ClipService.js').ClipService }} services
 */
export function handler(args, { clipService }) {
  InputValidator.assert(args, {
    projectId: { type: 'string', required: true, minLength: 1 },
    clipId:    { type: 'string', required: true, minLength: 1 },
  });

  const result = clipService.removeClip(args.projectId, args.clipId);
  return {
    ...result,
    message: `Clip "${result.clipId}" removed from track "${result.trackId}".`,
  };
}
